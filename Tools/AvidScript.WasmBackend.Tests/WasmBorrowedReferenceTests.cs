using System;
using System.IO;
using System.Linq;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class WasmBorrowedReferenceTests
{
    private const string I = "type:int32", V = "type:void", R = "node_ref", E = "erased", N = "node", Box = "box";
    private const string BI = "ref_i32", BR = "ref_node", BB = "ref_box", F = "borrow_callback";
    private const string Bool = "type:bool", BBool = "ref_bool";
    public static int Run()
    {
        GuestModule module = Create();
        Require(GuestModuleValidator.Validate(module).Succeeded, string.Join(" | ", GuestModuleValidator.Validate(module).Diagnostics.Select(item => item.Message)));
        WasmCompilationResult compiled = WasmModuleCompiler.Compile(module);
        Require(compiled.Succeeded, string.Join(" | ", compiled.Diagnostics.Select(item => item.Message)));
        Require(compiled.Bytes.SequenceEqual(WasmModuleCompiler.Compile(module).Bytes), "borrowed codegen deterministic");
        WasmCompilationResult cooperative = WasmModuleCompiler.Compile(module, new(true, 4));
        Require(cooperative.Succeeded && cooperative.CooperativeSafepointAttestation is { Verified: true }, "borrowed calls preserve cooperative safepoint proof");
        byte[] json = GuestIrSerializer.Serialize(module);
        Require(json.SequenceEqual(GuestIrSerializer.Serialize(GuestIrSerializer.Deserialize(json))), "borrowed contracts round trip");
        GuestModule[] invalid =
        {
            module with { SchemaVersion = 5, IrVersion = "1.4" },
            module with { Types = module.Types.Select(type => type.Id == BI ? type with { ElementTypeId = V } : type).ToArray() },
            module with { Types = module.Types.Select(type => type.Id == BI ? type with { Size = 8 } : type).ToArray() },
            module with { Types = module.Types.Select(type => type.Id == BI ? type with { Fields = type.Fields.Select(field => field.Id == "owner" ? field with { TypeId = R } : field).ToArray() } : type).ToArray() },
            module with { Types = module.Types.Append(new GuestType("escape", "struct", "memory", new[] { Field("escaped", BI) }, null, null, 16, 8)).ToArray() },
            module with { Types = module.Types.Append(new GuestType("escape", "array", "i32", Array.Empty<GuestField>(), BI, null, 4, 4)).ToArray() },
            module with { Globals = module.Globals.Append(new GuestGlobal("escape", BI, true, new("zero", null))).ToArray() },
            module with { Exports = module.Exports.Append(new GuestExport("escape", "forward")).ToArray() },
            module with { Imports = module.Imports.Append(new GuestImport("escape", "env", "escape", new[] { BI }, V)).ToArray() },
            module with { Functions = module.Functions.Append(Fn("escape", new[] { Reg("ref", BI) }, Array.Empty<GuestRegister>(), BI, Array.Empty<GuestInstruction>(), "ref")).ToArray() },
            ReplaceBegin(module, Op("borrow_address", "a", target: "a")),
            ReplaceBegin(module, Op("borrow_managed", "a", new[] { "owner" }, "absent")),
            ReplaceBegin(module, Op("borrow_managed", "a", new[] { "owner" }, "box")),
            ReplaceBegin(module, Op("borrow_field", "nested", new[] { "boxRef" }, "next")),
            ReplaceBegin(module, Op("borrow_load", "aliasResult", new[] { "stackRef" }, I)),
            ReplaceBegin(module, Op("borrow_store", args: new[] { "a", "nil" }, target: I)),
            ReplaceBegin(module, new("constant", "a", Array.Empty<string>(), null, null, new("zero", null))),
            ReplaceBegin(module, Op("field_load", "owner", new[] { "a" }, "owner")),
        };
        foreach (GuestModule candidate in invalid)
            Require(!GuestModuleValidator.Validate(candidate).Succeeded && !WasmModuleCompiler.Compile(candidate).Succeeded,
                "borrowed descriptor forgery or escape must be rejected");
        GuestModule trap = module with { Functions = module.Functions.Select(function => function.Id == "begin"
            ? function with
            {
                Locals = function.Locals.Concat(new[] { Reg("emptyBorrow", BI), Reg("invalidRead", I) }).ToArray(),
                Blocks = new[] { function.Blocks[0] with { Instructions = function.Blocks[0].Instructions
                    .Append(Op("borrow_load", "invalidRead", new[] { "emptyBorrow" }, I)).ToArray() } },
            } : function).ToArray() };
        WasmCompilationResult trapped = WasmModuleCompiler.Compile(trap);
        Require(trapped.Succeeded, "borrowed trap fixture compiles");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_MANAGED_HEAP_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output, "borrowed.wasm"), compiled.Bytes);
            File.WriteAllBytes(Path.Combine(output, "borrowed-trap.wasm"), trapped.Bytes);
            File.WriteAllBytes(Path.Combine(output, "borrowed.guest-ir.json"), json);
        }
        return invalid.Length + 6;
    }

    private static GuestModule Create()
    {
        GuestModule basis = WasmModuleCompilerTests.CreateMinimalModule();
        GuestType[] declarations = basis.Types.Concat(new[]
        {
            new GuestType(E, "managed_ref", "i64", Array.Empty<GuestField>(), null, null, 8, 8),
            new GuestType(R, "managed_ref", "i64", Array.Empty<GuestField>(), N, null, 8, 8),
            new GuestType(N, "struct", "memory", new[] { Field("value", I), Field("box", Box) }, null, null, 0, 1),
            new GuestType(Box, "struct", "memory", new[] { Field("value", I), Field("next", R) }, null, null, 0, 1),
            new GuestType(F, "function_ref", "i32", Array.Empty<GuestField>(), null, null, 4, 4),
            new GuestType(Bool, "scalar", "i32", Array.Empty<GuestField>(), null, null, 1, 1),
            GuestBorrowedReference.Declare(BI, I, E, I), GuestBorrowedReference.Declare(BR, R, E, I), GuestBorrowedReference.Declare(BB, Box, E, I),
            GuestBorrowedReference.Declare(BBool, Bool, E, I),
        }).ToArray();
        GuestTypeLayoutResult types = GuestDataLayout.ComputeTypes(declarations);
        Require(types.Succeeded, "borrowed layouts");
        GuestGlobal[] globals = { new("result", I, true, new("int32", "0")) };
        GuestLayoutResult layout = GuestLayoutBuilder.Build(types.Types, globals, Array.Empty<GuestDataSegment>());
        Require(layout.Succeeded && layout.Layout!.StateSlots.Single().Offset == 16, "borrowed result slot");
        GuestFunction read = Fn("read", new[] { Reg("owner", R) }, new[] { Reg("value", I) }, I,
            new[] { Op("managed_collect"), Op("managed_get", "value", new[] { "owner" }, "value") }, "value");
        GuestFunction alias = Fn("alias", new[] { Reg("a", BI), Reg("b", BI), Reg("owner", R) },
            new[] { Reg("old", I), Reg("one", I), Reg("next", I), Reg("reentered", I), Reg("second", I), Reg("sum", I) }, I, new[]
            {
                Op("borrow_load", "old", new[] { "a" }, I), Num("one", 1), Bin("next", "old", "one"),
                Op("borrow_store", args: new[] { "a", "next" }, target: I), Op("call", "reentered", new[] { "owner" }, "read"),
                Op("borrow_load", "second", new[] { "b" }, I), Bin("sum", "reentered", "second"),
            }, "sum");
        GuestFunction write = Fn("write", new[] { Reg("ref", BR) }, new[] { Reg("created", R), Reg("value", I) }, V, new[]
        {
            Num("value", 73), Op("managed_new", "created"), Op("managed_set", args: new[] { "created", "value" }, target: "value"),
            Op("borrow_store", args: new[] { "ref", "created" }, target: R),
        });
        GuestFunction forward = Fn("forward", new[] { Reg("ref", BR) }, new[] { Reg("observed", R), Reg("value", I) }, I, new[]
        {
            Op("call", args: new[] { "ref" }, target: "write"), Op("managed_collect"),
            Op("borrow_load", "observed", new[] { "ref" }, R), Op("managed_get", "value", new[] { "observed" }, "value"),
        }, "value");
        GuestFunction branch = new("branch", Array.Empty<GuestRegister>(),
            new[] { Reg("condition", Bool), Reg("truth", Bool), Reg("reference", BBool), Reg("good", I), Reg("bad", I) }, I, "entry", new[]
            {
                new GuestBasicBlock("entry", new[]
                {
                    new GuestInstruction("constant", "condition", Array.Empty<string>(), null, null, new("bool", "0")),
                    new GuestInstruction("constant", "truth", Array.Empty<string>(), null, null, new("bool", "1")),
                    Op("borrow_address", "reference", target: "condition"), Op("borrow_store", args: new[] { "reference", "truth" }, target: Bool),
                }, new("branch_if", "condition", "good", "bad", null)),
                new GuestBasicBlock("good", new[] { Num("good", 0) }, new("return", null, null, null, "good")),
                new GuestBasicBlock("bad", new[] { Num("bad", 1) }, new("return", null, null, null, "bad")),
            });
        GuestFunction begin = Fn("begin", Array.Empty<GuestRegister>(), new[]
        {
            Reg("owner", R), Reg("nil", R), Reg("a", BI), Reg("b", BI), Reg("boxRef", BB), Reg("nested", BI), Reg("callback", F),
            Reg("seven", I), Reg("thirtyNine", I), Reg("aliasResult", I), Reg("nestedResult", I), Reg("stack", R), Reg("stackRef", BR), Reg("stackResult", I),
            Reg("scalar", I), Reg("scalarRef", BI), Reg("twentyOne", I), Reg("one", I), Reg("scalarResult", I),
            Reg("box", Box), Reg("stackBox", BB), Reg("nextRef", BR), Reg("boxResult", I), Reg("s1", I), Reg("s2", I), Reg("s3", I), Reg("sum", I),
            Reg("branchResult", I), Reg("adjusted", I),
        }, V, new[]
        {
            Op("managed_new", "owner"), Num("seven", 7), Num("thirtyNine", 39), Num("twentyOne", 21), Num("one", 1),
            Op("borrow_managed", "a", new[] { "owner" }, "value"), Op("copy", "b", new[] { "a" }),
            Op("borrow_store", args: new[] { "a", "seven" }, target: I), Op("function_ref", "callback", target: "alias"),
            Op("call_indirect", "aliasResult", new[] { "callback", "a", "b", "owner" }, F),
            Op("borrow_managed", "boxRef", new[] { "owner" }, "box"), Op("borrow_field", "nested", new[] { "boxRef" }, "value"),
            Op("borrow_store", args: new[] { "nested", "thirtyNine" }, target: I),
            new GuestInstruction("constant", "nil", Array.Empty<string>(), null, null, new("null", null)), Op("local_store", args: new[] { "nil" }, target: "owner"),
            Op("managed_collect"), Op("borrow_load", "nestedResult", new[] { "nested" }, I),
            Op("managed_new", "stack"), Op("managed_set", args: new[] { "stack", "seven" }, target: "value"), Op("borrow_address", "stackRef", target: "stack"),
            Op("call", "stackResult", new[] { "stackRef" }, "forward"),
            Num("scalar", 10), Op("borrow_address", "scalarRef", target: "scalar"), Op("borrow_store", args: new[] { "scalarRef", "twentyOne" }, target: I),
            Bin("scalarResult", "scalar", "one"),
            new GuestInstruction("constant", "box", Array.Empty<string>(), null, null, new("zero", null)),
            Op("borrow_address", "stackBox", target: "box"), Op("borrow_field", "nextRef", new[] { "stackBox" }, "next"),
            Op("call", "boxResult", new[] { "nextRef" }, "forward"), Op("managed_collect"),
            Bin("s1", "aliasResult", "nestedResult"), Bin("s2", "s1", "stackResult"), Bin("s3", "s2", "scalarResult"), Bin("sum", "s3", "boxResult"),
            Op("call", "branchResult", target: "branch"), Bin("adjusted", "sum", "branchResult"),
            Op("global_store", args: new[] { "adjusted" }, target: "result"),
        });
        return basis with
        {
            SchemaVersion = 6, IrVersion = "1.5", Types = types.Types, Globals = globals, MemoryLayout = layout.Layout!,
            Imports = new[] { new GuestImport("heap", GuestManagedHeap.ImportModule, GuestManagedHeap.ImportName, new[] { I, I, I, I }, I) },
            Functions = new[] { begin, read, alias, write, forward, branch }, Exports = new[] { new GuestExport("avid_on_begin_play", "begin") },
            FunctionReferences = new[] { new GuestFunctionReference(F, new[] { BI, BI, R }, I, new[] { "alias" }) },
        };
    }

    private static GuestModule ReplaceBegin(GuestModule module, GuestInstruction replacement) => module with
    { Functions = module.Functions.Select(function => function.Id != "begin" ? function : function with
        { Blocks = new[] { function.Blocks[0] with { Instructions = new[] { replacement } } } }).ToArray() };
    private static GuestField Field(string id, string type) => new(id, id, type, 0);
    private static GuestRegister Reg(string id, string type) => new(id, type);
    private static GuestInstruction Op(string op, string? result = null, string[]? args = null, string? target = null) => new(op, result, args ?? Array.Empty<string>(), target, null, null);
    private static GuestInstruction Num(string result, int n) => new("constant", result, Array.Empty<string>(), null, null, new("int32", n.ToString(System.Globalization.CultureInfo.InvariantCulture)));
    private static GuestInstruction Bin(string result, string a, string b) => new("binary", result, new[] { a, b }, null, "add", null);
    private static GuestFunction Fn(string id, GuestRegister[] parameters, GuestRegister[] locals, string result, GuestInstruction[] ops, string? value = null) =>
        new(id, parameters, locals, result, "entry", new[] { new GuestBasicBlock("entry", ops, new("return", null, null, null, value)) });
    private static void Require(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }
}
