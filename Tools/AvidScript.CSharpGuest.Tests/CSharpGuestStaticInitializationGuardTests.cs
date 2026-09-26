using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using AvidScript.CSharpGuest;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

// Tests the compiler's executable guard composition independently of source
// triggers. Native Automation executes these bytes on both production backends.
internal static class CSharpGuestStaticInitializationGuardTests
{
    private const string I = "type:int32", V = "type:void", B = "type:bool", F = "type:float32";
    private const string R = "type:language_error_root", P = "type:language_error_payload", O = "type:outcome:void";
    private static string Ensure(string name) => CSharpStaticInitializationGuards.FunctionId("type:" + name);
    private static CSharpStaticInitializer[] Initializers() => new[] {
        Init("Once", "once_body"), Init("A", "a_body"), Init("B", "b_body"), Init("Reentry", "reentry_body"),
        Init("Cache<int>", "generic_body"), Init("Cache<long>", "generic_body"), Init("Broken", "broken_body"), Init("Outer", "outer_body"),
    };
    private static CSharpStaticInitializer Init(string type, string body) => new("type:" + type, body, 1);

    public static int Run()
    {
        int count = 0;
        void Check(bool condition, string message) { if (!condition) throw new InvalidOperationException("Static guards: " + message); count++; }
        GuestModule input = Fixture();
        Check(CSharpStaticInitializationGuards.TryCompose(input, Initializers(), out var composed, out string? error), error ?? "composition");
        GuestModule module = composed!;
        Check(module.SchemaVersion == 27 && module.StaticStorage!.BaseSchemaVersion == 17, "storage wraps exact error profile");
        Check(module.StaticStorage!.Slots.Count == 9, "each closed type has one state slot and existing slots remain");
        Check(module.Globals.SequenceEqual(input.Globals) && module.MemoryLayout.StateSlots.All(slot => !slot.GlobalId.Contains("$initialization", StringComparison.Ordinal)), "initializer states do not enter migratable globals");
        Check(module.Types.Single(type => type.Id == P).Fields.Any(field => field.Id == "field:inner_root" && field.TypeId == R), "inner exception is a traced reference");
        byte[] bytes = GuestIrSerializer.Serialize(module);
        Check(GuestModuleValidator.Validate(GuestIrSerializer.Deserialize(bytes)).Succeeded, "serialized IR validation");
        Check(CSharpStaticInitializationGuards.TryCompose(input, Initializers(), out var repeated, out _)
            && bytes.SequenceEqual(GuestIrSerializer.Serialize(repeated!)), "deterministic composition");
        foreach (var invalid in new[] {
            Array.Empty<CSharpStaticInitializer>(), new[] { Init("Once", "missing") },
            new[] { Init("Once", "begin") }, new[] { Init("Once", "once_body"), Init("Once", "once_body") },
            new[] { Init("Once", "once_body") with { SourceToken = 999 } }, new CSharpStaticInitializer[] { null! },
        }) Check(!CSharpStaticInitializationGuards.TryCompose(input, invalid, out var result, out _) && result is null, "invalid initializer declaration reached publication");
        foreach (var invalid in new[] {
            input with { LanguageErrorCatalog = null }, input with { LanguageOutcomeTypes = null },
            input with { SchemaVersion = 17, IrVersion = "1.16" }, input with { StaticStorage = null },
            input with { StaticStorage = input.StaticStorage! with { Slots = null! } },
            input with { StaticStorage = input.StaticStorage! with { Slots = new GuestStaticSlot[] { null! } } },
            input with { StaticStorage = input.StaticStorage! with { BaseSchemaVersion = 16, BaseIrVersion = "1.15" } },
            input with { Functions = input.Functions.Append(input.Functions[0] with { Id = Ensure("Once") }).ToArray() },
            input with { Types = input.Types.Where(type => type.Id != R).ToArray() },
        }) Check(!CSharpStaticInitializationGuards.TryCompose(invalid, Initializers(), out var result, out _) && result is null, "invalid module reached publication");
        string? directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_STATIC_GUARD_WASM_DIR");
        foreach (bool cooperative in new[] { false, true })
        {
            var wasm = WasmModuleCompiler.Compile(module, new(cooperative, 4));
            Check(wasm.Succeeded, string.Join(" | ", wasm.Diagnostics.Select(item => item.Message)));
            Check(wasm.Bytes.SequenceEqual(WasmModuleCompiler.Compile(GuestIrSerializer.Deserialize(bytes), new(cooperative, 4)).Bytes), "deterministic WASM round trip");
            if (!string.IsNullOrWhiteSpace(directory))
            {
                Directory.CreateDirectory(directory);
                string name = "static-initialization" + (cooperative ? "-cooperative" : "");
                File.WriteAllBytes(Path.Combine(directory, name + ".wasm"), wasm.Bytes);
                File.WriteAllBytes(Path.Combine(directory, name + ".guest-ir.json"), bytes);
            }
        }
        return count;
    }

    private static GuestModule Fixture()
    {
        GuestType[] rawTypes = {
            new(V, "void", "none", Array.Empty<GuestField>(), null, null, 0, 1),
            new(I, "scalar", "i32", Array.Empty<GuestField>(), null, null, 4, 4),
            new(B, "scalar", "i32", Array.Empty<GuestField>(), null, null, 1, 1),
            new(F, "scalar", "f32", Array.Empty<GuestField>(), null, null, 4, 4),
            new(P, "struct", "memory", new[] { new GuestField("field:code", "code", I, 0) }, null, null, 0, 1),
            new(R, "managed_ref", "i64", Array.Empty<GuestField>(), P, null, 8, 8),
            new(O, "struct", "memory", new[] { new GuestField("field:status", "status", I, 0),
                new GuestField("field:error_type", "error_type", I, 0), new GuestField("field:source", "source", I, 0),
                new GuestField("field:error_root", "error_root", R, 0) }, null, null, 0, 1),
        };
        var types = GuestDataLayout.ComputeTypes(rawTypes).Types;
        var globals = new[] { "once_count", "a_value", "b_value", "first", "later", "generic_count", "attempts" }
            .Select(name => new GuestGlobal(name, I, true, new("zero", null))).ToArray();
        var layout = GuestLayoutBuilder.Build(types, globals, Array.Empty<GuestDataSegment>());
        List<GuestFunction> functions = new() {
            Simple("begin", V, Array.Empty<GuestRegister>(), Array.Empty<GuestInstruction>()),
            new("tick", new[] { Reg("dt", F) }, Array.Empty<GuestRegister>(), V, "entry", new[] { Block("entry", Array.Empty<GuestInstruction>(), Return()) }),
            SuccessBody("once_body", Increment("once_count")), SuccessBody("generic_body", Increment("generic_count")),
            DependentBody("a_body", "B", new[] { Op("global_load", "n", target: "b_value"), Constant("one", 1), Binary("sum", "n", "one", "add"), Op("global_store", operands: new[] { "sum" }, target: "a_value") }),
            DependentBody("b_body", "A", new[] { Op("global_load", "n", target: "a_value"), Constant("one", 2), Binary("sum", "n", "one", "add"), Op("global_store", operands: new[] { "sum" }, target: "b_value") }),
            DependentBody("reentry_body", "Reentry", new[] { Op("global_load", "n", target: "later"), Op("global_store", operands: new[] { "n" }, target: "first"), Constant("one", 7), Op("global_store", operands: new[] { "one" }, target: "later") }),
            DependentBody("outer_body", "Broken", Array.Empty<GuestInstruction>()),
            BrokenBody(),
            Probe("once", new[] { "Once" }, new[] { Op("global_load", "value", target: "once_count") }),
            Probe("cycle", new[] { "A", "B" }, CombineGlobals("a_value", "b_value", 10)),
            Probe("reentry", new[] { "Reentry" }, CombineGlobals("first", "later", 10)),
            Probe("generic", new[] { "Cache<int>", "Cache<long>" }, new[] { Op("global_load", "value", target: "generic_count") }),
            FailureProbe("failure", "Broken", nested: false), FailureProbe("nested_failure", "Outer", nested: true),
            Simple("read_attempts", I, new[] { Reg("value", I) }, new[] { Op("global_load", "value", target: "attempts") }, "value"),
        };
        string hash = new('a', 64);
        return new(27, "1.26", "static-initialization-guards", "guest-ir",
            new("Fixtures/StaticInitializationGuards", hash, hash, hash, 34, "1.43"), true, layout.Layout!, types,
            new[] { new GuestImport("import:heap", GuestManagedHeap.ImportModule, GuestManagedHeap.ImportName, new[] { I, I, I, I }, I) },
            globals, layout.DataSegments, functions,
            new[] { new GuestExport("avid_on_begin_play", "begin"), new GuestExport("avid_on_tick", "tick") }
                .Concat(new[] { "once", "cycle", "reentry", "generic", "failure", "nested_failure", "attempts" }.Select(name => new GuestExport(name, name == "attempts" ? "read_attempts" : name))).ToArray(),
            Array.Empty<GuestDiagnostic>())
        {
            StaticStorage = new(17, "1.16", new[] { new GuestStaticSlot("static:observed_failure", R) }),
            LanguageOutcomeTypes = new[] { new GuestLanguageOutcomeType(O, null) },
            LanguageErrorCatalog = new(new[] { new GuestLanguageErrorTypeToken(1, "type:global::System.InvalidOperationException"),
                new GuestLanguageErrorTypeToken(2, CSharpStaticInitializationGuards.ExceptionType) },
                new[] { new GuestLanguageErrorSourceToken(1, "Fixtures/StaticInitializationGuards", 100, 0, 1, 0, 0, 0, 1) }),
        };
    }

    private static GuestFunction SuccessBody(string id, GuestInstruction[] statements) => Simple(id, O,
        new[] { Reg("n", I), Reg("one", I), Reg("sum", I), Reg("zero", I), Reg("out", O) },
        statements.Concat(Success()).ToArray(), "out");
    private static GuestFunction DependentBody(string id, string type, GuestInstruction[] statements) => new(id,
        Array.Empty<GuestRegister>(), new[] { Reg("dependency", O), Reg("status", I), Reg("n", I), Reg("one", I), Reg("sum", I), Reg("zero", I), Reg("out", O) }, O, "entry", new[] {
            Block("entry", new[] { Op("call", "dependency", target: Ensure(type)), Load("status", "dependency", "status") }, Branch("status", "failed", "ok")),
            Block("failed", Array.Empty<GuestInstruction>(), Return("dependency")),
            Block("ok", statements.Concat(Success()).ToArray(), Return("out")),
        });
    private static GuestFunction BrokenBody() => Simple("broken_body", O,
        new[] { Reg("n", I), Reg("one", I), Reg("sum", I), Reg("out", O), Reg("root", R), Reg("code", I) },
        Increment("attempts").Concat(new[] { Constant("code", 1), Op("managed_new", "root"), Set("root", "code", "code"),
            Op("stack_alloc", "out"), Store("out", "status", "code"), Store("out", "error_type", "code"),
            Store("out", "source", "code"), Store("out", "error_root", "root") }).ToArray(), "out");
    private static GuestFunction Probe(string id, string[] guards, GuestInstruction[] statements)
    {
        var locals = new[] { Reg("value", I), Reg("n", I), Reg("scale", I), Reg("product", I), Reg("m", I) }.ToList();
        List<GuestBasicBlock> blocks = new();
        for (int index = 0; index < guards.Length; index++)
        {
            string result = "result" + index, status = "status" + index;
            locals.Add(Reg(result, O)); locals.Add(Reg(status, I));
            blocks.Add(Block("guard" + index, new[] { Op("call", result, target: Ensure(guards[index])), Load(status, result, "status") },
                Branch(status, "failed", index == guards.Length - 1 ? "read" : "guard" + (index + 1))));
        }
        blocks.Add(Block("failed", Array.Empty<GuestInstruction>(), new("trap", null, null, null, null)));
        blocks.Add(Block("read", statements, Return("value")));
        return new(id, Array.Empty<GuestRegister>(), locals, I, "guard0", blocks);
    }
    private static GuestFunction FailureProbe(string id, string type, bool nested)
    {
        List<GuestInstruction> inspect = new() { Load("root", "result", "error_root"), Load("outer_code", "result", "error_type"),
            Get("inner", "root", "inner_root"), Get("inner_code", "root", "inner_type"), Get("inner_source", "root", "inner_source"),
            Load("source", "result", "source"), Op("managed_collect") };
        if (nested) { inspect.Add(Get("leaf", "inner", "inner_root")); inspect.Add(Get("leaf_code", "leaf", "code")); }
        else inspect.Add(Get("leaf_code", "inner", "code"));
        inspect.AddRange(new[] { Constant("hundred", 100), Constant("ten", 10), Binary("outer_score", "outer_code", "hundred", "multiply"),
            Binary("inner_score", "inner_code", "ten", "multiply"), Binary("partial", "outer_score", "inner_score", "add"), Binary("value", "partial", "leaf_code", "add"),
            Constant("one", 1), Binary("source_ok", "source", "one", "equals"), Binary("inner_source_ok", "inner_source", "one", "equals"),
            Binary("sources_ok", "source_ok", "inner_source_ok", "bitwise_and") });
        var locals = new[] { Reg("result", O), Reg("status", I), Reg("root", R), Reg("inner", R), Reg("leaf", R), Reg("previous", R), Reg("empty", R),
            Reg("first_call", B), Reg("same", B), Reg("source_ok", B), Reg("inner_source_ok", B), Reg("sources_ok", B) }
            .Concat(new[] { "outer_code", "inner_code", "leaf_code", "inner_source", "source", "hundred", "ten", "outer_score", "inner_score", "partial", "value", "one" }.Select(name => Reg(name, I))).ToArray();
        List<GuestBasicBlock> blocks = new() {
            Block("entry", new[] { Op("call", "result", target: Ensure(type)), Load("status", "result", "status") }, Branch("status", "inspect", "invalid")),
            Block("inspect", inspect.ToArray(), Branch("sources_ok", nested ? "return" : "identity", "invalid")),
            Block("invalid", Array.Empty<GuestInstruction>(), new("trap", null, null, null, null)),
            Block("return", Array.Empty<GuestInstruction>(), Return("value")),
        };
        if (!nested) blocks.AddRange(new[] {
            Block("identity", new[] { Op(GuestStaticStorage.GetOp, "previous", target: "static:observed_failure"),
                new GuestInstruction("constant", "empty", Array.Empty<string>(), null, null, new("null", null)), Binary("first_call", "previous", "empty", "equals") }, Branch("first_call", "remember", "compare")),
            Block("remember", new[] { Op(GuestStaticStorage.SetOp, operands: new[] { "root" }, target: "static:observed_failure") }, new("branch", null, "return", null, null)),
            Block("compare", new[] { Binary("same", "previous", "root", "equals") }, Branch("same", "return", "invalid")),
        });
        return new(id, Array.Empty<GuestRegister>(), locals, I, "entry", blocks);
    }
    private static GuestInstruction[] Increment(string global) => new[] { Op("global_load", "n", target: global), Constant("one", 1), Binary("sum", "n", "one", "add"), Op("global_store", operands: new[] { "sum" }, target: global) };
    private static GuestInstruction[] CombineGlobals(string first, string second, int scale) => new[] { Op("global_load", "n", target: first), Op("global_load", "m", target: second), Constant("scale", scale), Binary("product", "n", "scale", "multiply"), Binary("value", "product", "m", "add") };
    private static GuestInstruction[] Success() => new[] { Op("stack_alloc", "out"), Constant("zero", 0), Store("out", "status", "zero") };
    private static GuestFunction Simple(string id, string type, GuestRegister[] locals, GuestInstruction[] instructions, string? value = null) => new(id, Array.Empty<GuestRegister>(), locals, type, "entry", new[] { Block("entry", instructions, Return(value)) });
    private static GuestRegister Reg(string id, string type) => new(id, type);
    private static GuestBasicBlock Block(string id, GuestInstruction[] instructions, GuestTerminator end) => new(id, instructions, end);
    private static GuestTerminator Return(string? value = null) => new("return", null, null, null, value);
    private static GuestTerminator Branch(string condition, string yes, string no) => new("branch_if", condition, yes, no, null);
    private static GuestInstruction Op(string op, string? result = null, string[]? operands = null, string? target = null) => new(op, result, operands ?? Array.Empty<string>(), target, null, null);
    private static GuestInstruction Constant(string id, int value) => new("constant", id, Array.Empty<string>(), null, null, new("int32", value.ToString(CultureInfo.InvariantCulture)));
    private static GuestInstruction Binary(string id, string a, string b, string kind) => new("binary", id, new[] { a, b }, null, kind, null);
    private static GuestInstruction Load(string id, string owner, string field) => Op("field_load", id, new[] { owner }, "field:" + field);
    private static GuestInstruction Store(string owner, string field, string value) => Op("field_store", operands: new[] { owner, value }, target: "field:" + field);
    private static GuestInstruction Get(string id, string owner, string field) => Op("managed_get", id, new[] { owner }, "field:" + field);
    private static GuestInstruction Set(string owner, string field, string value) => Op("managed_set", operands: new[] { owner, value }, target: "field:" + field);
}
