#include "../compiler.h"
#include "../ir_interpreter.h"
#include "../ir_verifier.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace gdscript;

static void check(bool condition, const std::string& message) {
	if (!condition) throw std::runtime_error(message);
}

int main() try {
	Compiler compiler;
	for (bool optimize : {false, true}) {
		CompilerOptions options;
		options.optimize = optimize;
		auto ir = compiler.compile_to_ir(
			"@tool\nclass_name FrontendExample\n"
			"var count: int = 7\n"
			"func answer(value: int) -> int:\n\treturn value * 2 + 2\n", options);
		check(ir.has_value(), compiler.get_error());
		ir_verify(*ir);
		IRInterpreter interpreter(*ir);
		check(std::get<int64_t>(interpreter.call("answer", {int64_t(20)})) == 42,
			"Optimized/unoptimized frontend semantics differ");
		check(compiler.is_tool() && compiler.get_class_name() == "FrontendExample", "Lost script metadata");
		check(compiler.get_property_signatures().size() == 1 && ir->properties.size() == 1, "Lost property metadata");
		check(!compiler.get_function_signatures().empty(), "Lost signatures");
		check(compiler.get_line_table().entries.empty(), "Frontend emitted machine addresses");
	}

	auto invalid = compiler.compile_to_ir("func broken(:\n\tpass\n");
	check(!invalid && compiler.get_error_info().has_error && !compiler.get_error().empty(), "Missing diagnostics");
	check(!compiler.is_tool() && compiler.get_class_name().empty(), "Stale script metadata after failure");
	auto empty = compiler.compile_to_ir("");
	check(empty.has_value() && !compiler.get_error_info().has_error && compiler.get_error().empty(),
		"Empty success must differ from failure and clear diagnostics");

	auto implicit_await = compiler.compile_to_ir(
		"@auto_await\n"
		"func wait_for(sig):\n\treturn await sig\n"
		"func run(sig):\n\twait_for(sig)\n\treturn 1\n"
		"func nested(sig, condition):\n"
		"\tif condition:\n"
		"\t\tfor i in range(1):\n"
		"\t\t\twait_for(sig)\n"
		"\treturn 1\n");
	check(implicit_await.has_value(), compiler.get_error());
	ir_verify(*implicit_await);
	const auto run = std::find_if(implicit_await->functions.begin(), implicit_await->functions.end(),
		[](const IRFunction& function) { return function.name == "run"; });
	check(run != implicit_await->functions.end() && run->is_coroutine,
		"An implicit await did not make its caller a coroutine");
	if (run != implicit_await->functions.end()) {
		check(std::count_if(run->instructions.begin(), run->instructions.end(),
			[](const IRInstruction& instruction) { return instruction.opcode == IROpcode::AWAIT; }) == 1,
			"An annotated call did not lower to an await");
	}
	const auto nested = std::find_if(implicit_await->functions.begin(), implicit_await->functions.end(),
		[](const IRFunction& function) { return function.name == "nested"; });
	check(nested != implicit_await->functions.end() && nested->is_coroutine,
		"An implicit await inside a branch and loop did not make its caller a coroutine");
	if (nested != implicit_await->functions.end()) {
		check(std::count_if(nested->instructions.begin(), nested->instructions.end(),
			[](const IRInstruction& instruction) { return instruction.opcode == IROpcode::AWAIT; }) == 1,
			"An annotated call inside a branch and loop did not lower to an await");
	}
	check(!compiler.compile_to_ir(
		"@auto_await\n"
		"func wait_for(sig):\n\treturn await sig\n"
		"@test\n"
		"func check_wait():\n\twait_for(null)\n"),
		"An @test function became a coroutine through @auto_await");

	CompilerOptions options;
	options.emit_tests = false;
	auto shipping = compiler.compile_to_ir("@test\nfunc check_answer():\n\tpass\nfunc answer():\n\treturn 42\n", options);
	check(shipping.has_value(), compiler.get_error());
	check(shipping->tests.empty() && std::none_of(shipping->functions.begin(), shipping->functions.end(),
		[](const IRFunction& function) { return function.name == "check_answer"; }), "Frontend retained shipping tests");

	options = {};
	options.base_sources.push_back({"Base", "res://base.sgd", "func inherited() -> int:\n\treturn 42\n", false});
	auto derived = compiler.compile_to_ir("extends Base\nfunc answer() -> int:\n\treturn inherited()\n", options);
	check(derived.has_value(), compiler.get_error());
	ir_verify(*derived);
	IRInterpreter inherited(*derived);
	check(std::get<int64_t>(inherited.call("answer")) == 42, "Frontend skipped base merging");
	options.restricted = true;
	check(!compiler.compile_to_ir("extends Base\n", options), "Frontend skipped restricted policy");
	for (bool optimize : {false, true}) {
		CompilerOptions editor;
		editor.native_classes = true;
		editor.optimize = optimize;
		auto compiled = compiler.compile_to_ir(
			"extends RefCounted\n"
			"var names: PackedStringArray = []\n"
			"var optional: PackedStringArray? = []\n"
			"class Row extends RefCounted:\n"
			"\tconst Data = preload(\"res://data.ugd\")\n"
			"\tsignal clicked(index: int)\n"
			"\tfunc trigger(index: int):\n\t\tclicked.emit(index)\n"
			"func axis():\n\treturn Vector3.AXIS_Z + Vector4i.AXIS_W\n", editor);
		check(compiled.has_value(), compiler.get_error());
		ir_verify(*compiled);
		auto axis_ir = compiler.compile_to_ir("func axis():\n\treturn Vector3.AXIS_Z + Vector4i.AXIS_W\n", editor);
		check(axis_ir.has_value(), compiler.get_error());
		IRInterpreter axes(*axis_ir);
		check(std::get<int64_t>(axes.call("axis")) == 5, "Builtin integer constants changed values");
		check(compiled->has_member_init && compiled->has_global_init,
			"Packed members and nested preloads need their respective initializers");
		check(compiled->class_signatures.size() == 1 &&
			compiled->class_signatures[0].signals.size() == 1,
			"Nested class signal metadata missing");
		auto encoded = encode_class_signatures(compiled->class_signatures);
		std::vector<ClassSignature> decoded;
		check(decode_class_signatures(encoded.data(), encoded.size(), decoded), "Class metadata round trip");
		check(decoded.size() == 1 && decoded[0].signals.size() == 1 &&
			decoded[0].signals[0].name == "clicked" &&
			decoded[0].signals[0].parameters[0].type == Variant::INT,
			"Class metadata lost its signal signature");
		check(!decode_class_signatures(encoded.data(), encoded.size() - 1, decoded) && decoded.empty(),
			"Truncated class signal metadata accepted");
		check(!compiler.compile_to_ir("class Row:\n\tsignal hit\n\tvar hit = 1\n", editor),
			"Nested signal and field collision accepted");
		check(!compiler.compile_to_ir("class Row:\n\tsignal hit\n\tsignal hit\n", editor),
			"Duplicate nested signal accepted");
	}

	std::cout << "Standalone frontend tests passed\n";
	return 0;
} catch (const std::exception& error) {
	std::cerr << error.what() << '\n';
	return 1;
}
