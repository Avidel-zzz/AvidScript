using System;
using System.IO;
using System.Linq;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

// A backend probe for the C3 language-error ABI. Each call returns its own
// value/status/source tuple through SRet; no module-global error slot is used.
internal static class WasmOutcomeSlotTests
{
    private const string Int = "type:int32";
    private const string Outcome = "type:language_outcome";
    private const string Leaf = "function:outcome_leaf";
    private const string Caller = "function:outcome_caller";

    public static int Run()
    {
        GuestModule module = Create();
        GuestValidationResult validation = GuestModuleValidator.Validate(module);
        Check(validation.Succeeded, string.Join(" | ", validation.Diagnostics.Select(item => item.Message)));
        byte[] json = GuestIrSerializer.Serialize(module);
        Check(json.SequenceEqual(GuestIrSerializer.Serialize(GuestIrSerializer.Deserialize(json))),
            "outcome slot fixture must round-trip canonically");
        WasmCompilationResult compiled = WasmModuleCompiler.Compile(module);
        Check(compiled.Succeeded, string.Join(" | ", compiled.Diagnostics.Select(item => item.Message)));
        Check(compiled.Bytes.SequenceEqual(WasmModuleCompiler.Compile(module).Bytes),
            "outcome slot WASM must be deterministic");

        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_OUTCOME_SLOT_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output, "outcome-slots.wasm"), compiled.Bytes);
        }
        return 4;
    }

    private static GuestModule Create()
    {
        GuestModule baseline = WasmModuleCompilerTests.CreateMinimalModule();
        GuestType declared = new(Outcome, "struct", "memory", new[]
        {
            Field("status", 0), Field("error_type", 4), Field("source", 8),
            Field("value", 12), Field("cleanup", 16),
        }, null, null, 0, 1);
        GuestTypeLayoutResult types = GuestDataLayout.ComputeTypes(baseline.Types.Append(declared).ToArray());
        Check(types.Succeeded, "outcome slot type layout");
        GuestType outcome = types.Types.Single(type => type.Id == Outcome);
        Check(outcome.Size == 20 && outcome.Alignment == 4
            && outcome.Fields.Select(field => field.Offset).SequenceEqual(new[] { 0, 4, 8, 12, 16 }),
            "outcome slot offsets must be canonical");
        GuestLayoutResult layout = GuestLayoutBuilder.Build(
            types.Types, Array.Empty<GuestGlobal>(), Array.Empty<GuestDataSegment>());
        Check(layout.Succeeded && layout.Layout is not null, "outcome slot memory layout");
        return baseline with
        {
            SchemaVersion = GuestModuleValidator.CurrentSchemaVersion,
            IrVersion = GuestModuleValidator.CurrentIrVersion,
            Types = types.Types,
            MemoryLayout = layout.Layout!,
            Functions = new[] { CreateLeaf(), CreateCaller(), CreateBegin() },
            Exports = new[] { new GuestExport("outcome_call", Caller),
                new GuestExport("avid_on_begin_play", "function:outcome_begin") },
        };
    }

    private static GuestFunction CreateBegin() =>
        new("function:outcome_begin", Array.Empty<GuestRegister>(),
            Array.Empty<GuestRegister>(), "type:void", "entry",
            new[] { Block("entry", Array.Empty<GuestInstruction>(),
                new GuestTerminator("return", null, null, null, null)) });

    private static GuestFunction CreateLeaf()
    {
        GuestRegister[] locals =
        {
            Reg("out", Outcome), Reg("zero", Int), Reg("one", Int),
            Reg("error_type", Int), Reg("source", Int), Reg("ten", Int),
            Reg("has_value", Int), Reg("value", Int),
        };
        return new GuestFunction(Leaf, new[] { Reg("input", Int) }, locals, Outcome, "entry",
            new[]
            {
                Block("entry", new[]
                {
                    Op("stack_alloc", "out"),
                    Constant("zero", 0), Constant("one", 1),
                    Constant("error_type", 7), Constant("source", 31),
                    Constant("ten", 10),
                    new GuestInstruction("binary", "has_value", new[] { "input", "zero" },
                        null, "greater_than", null),
                }, BranchIf("has_value", "success", "error")),
                Block("success", new[]
                {
                    new GuestInstruction("binary", "value", new[] { "input", "ten" },
                        null, "add", null),
                    Store("status", "zero"), Store("error_type", "zero"),
                    Store("source", "zero"), Store("value", "value"),
                    Store("cleanup", "zero"),
                }, Return("out")),
                Block("error", new[]
                {
                    Store("status", "one"), Store("error_type", "error_type"),
                    Store("source", "source"), Store("value", "zero"),
                    Store("cleanup", "zero"),
                }, Return("out")),
            });
    }

    private static GuestFunction CreateCaller()
    {
        GuestRegister[] locals =
        {
            Reg("out", Outcome), Reg("first", Outcome), Reg("second", Outcome),
            Reg("zero", Int), Reg("one", Int), Reg("replacement_type", Int),
            Reg("replacement_source", Int), Reg("first_status", Int),
            Reg("second_status", Int), Reg("next", Int),
            Reg("first_value", Int), Reg("second_value", Int), Reg("sum", Int),
            Reg("first_type", Int), Reg("first_source", Int),
            Reg("second_type", Int), Reg("second_source", Int),
        };
        return new GuestFunction(Caller,
            new[] { Reg("input", Int), Reg("cleanup_throws", Int) },
            locals, Outcome, "entry",
            new[]
            {
                Block("entry", new[]
                {
                    Op("stack_alloc", "out"),
                    Constant("zero", 0), Constant("one", 1),
                    Constant("replacement_type", 9),
                    Constant("replacement_source", 44),
                    Op("call", "first", new[] { "input" }, Leaf),
                    Load("first_status", "first", "status"),
                }, BranchIf("first_status", "first_error", "second_call")),
                Block("second_call", new[]
                {
                    new GuestInstruction("binary", "next", new[] { "input", "one" },
                        null, "subtract", null),
                    Op("call", "second", new[] { "next" }, Leaf),
                    Load("second_status", "second", "status"),
                }, BranchIf("second_status", "second_error", "success")),
                Block("success", new[]
                {
                    Load("first_value", "first", "value"),
                    Load("second_value", "second", "value"),
                    new GuestInstruction("binary", "sum",
                        new[] { "first_value", "second_value" }, null, "add", null),
                    Store("status", "zero"), Store("error_type", "zero"),
                    Store("source", "zero"), Store("value", "sum"),
                    Store("cleanup", "one"),
                }, BranchIf("cleanup_throws", "replace", "done")),
                Block("first_error", new[]
                {
                    Load("first_type", "first", "error_type"),
                    Load("first_source", "first", "source"),
                    Store("status", "one"), Store("error_type", "first_type"),
                    Store("source", "first_source"), Store("value", "zero"),
                    Store("cleanup", "one"),
                }, BranchIf("cleanup_throws", "replace", "done")),
                Block("second_error", new[]
                {
                    Load("second_type", "second", "error_type"),
                    Load("second_source", "second", "source"),
                    Store("status", "one"), Store("error_type", "second_type"),
                    Store("source", "second_source"), Store("value", "zero"),
                    Store("cleanup", "one"),
                }, BranchIf("cleanup_throws", "replace", "done")),
                Block("replace", new[]
                {
                    Store("status", "one"),
                    Store("error_type", "replacement_type"),
                    Store("source", "replacement_source"),
                    Store("value", "zero"),
                }, new GuestTerminator("branch", null, "done", null, null)),
                Block("done", Array.Empty<GuestInstruction>(), Return("out")),
            });
    }

    private static GuestField Field(string name, int offset) =>
        new("field:" + name, name, Int, offset);

    private static GuestRegister Reg(string id, string type) => new(id, type);

    private static GuestBasicBlock Block(string id, GuestInstruction[] instructions,
        GuestTerminator terminator) => new(id, instructions, terminator);

    private static GuestInstruction Op(string op, string? result, string[]? operands = null,
        string? target = null) =>
        new(op, result, operands ?? Array.Empty<string>(), target, null, null);

    private static GuestInstruction Constant(string result, int value) =>
        new("constant", result, Array.Empty<string>(), null, null,
            new GuestConstant("int32", value.ToString(System.Globalization.CultureInfo.InvariantCulture)));

    private static GuestInstruction Store(string field, string value) =>
        Op("field_store", null, new[] { "out", value }, "field:" + field);

    private static GuestInstruction Load(string result, string source, string field) =>
        Op("field_load", result, new[] { source }, "field:" + field);

    private static GuestTerminator BranchIf(string condition, string yes, string no) =>
        new("branch_if", condition, yes, no, null);

    private static GuestTerminator Return(string value) =>
        new("return", null, null, null, value);

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}
