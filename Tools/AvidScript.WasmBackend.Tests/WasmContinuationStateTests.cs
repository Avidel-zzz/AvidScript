using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class WasmContinuationStateTests
{
    private const string I = "type:int32", L = "type:int64", F = "type:float32", V = "type:void";
    private const string R = "type:node_ref", S = "type:state_ref", W = "type:wrong_ref", E = "type:erased";

    public static int Run()
    {
        GuestModule module = Create();
        byte[] json = GuestIrSerializer.Serialize(module);
        Check(json.SequenceEqual(GuestIrSerializer.Serialize(GuestIrSerializer.Deserialize(json))), "state IR round trip");
        WasmCompilationResult compiled = Compile(module);
        Check(compiled.Bytes.SequenceEqual(Compile(module).Bytes), "state codegen determinism");
        Check(WasmArtifactInspector.Inspect(compiled.Bytes).Imports.Count == 4, "no undeclared state imports");
        GuestModule wrongRead = module with { Functions = module.Functions.Select(fn => fn.Id != "resume" ? fn : fn with
        { Locals = fn.Locals.Select(reg => reg.Id == "state" ? reg with { TypeId = W } : reg).ToArray() }).ToArray() };
        WasmCompilationResult wrong = Compile(wrongRead);
        GuestInstruction store = module.Functions[0].Blocks[0].Instructions.First(op => op.Op == GuestContinuationState.StoreOp);
        GuestInstruction read = module.Functions[1].Blocks[0].Instructions[0];
        List<GuestModule> invalid = new()
        {
            module with { SchemaVersion = 10, IrVersion = "1.9" },
            module with { SchemaVersion = 12, IrVersion = "1.11" },
            module with { IrVersion = "1.9" },
            module with { Imports = module.Imports.Where(import => import.Id != "store").ToArray() },
            module with { Imports = module.Imports.Where(import => import.Id != "heap").ToArray() },
            Replace(module, "begin", store, store with { TargetId = "read" }),
            Replace(module, "begin", store, store with { TargetId = null }),
            Replace(module, "begin", store, store with { ResultId = null }),
            Replace(module, "begin", store, store with { ResultId = "state" }),
            Replace(module, "begin", store, store with { OperandIds = Array.Empty<string>() }),
            Replace(module, "begin", store, store with { OperandIds = new[] { "number", "state" } }),
            Replace(module, "begin", store, store with { OperandIds = new[] { "token", "number" } }),
            Replace(module, "begin", store, store with { OperandIds = new[] { "token", "erased" } }),
            Replace(module, "begin", store, store with { Constant = new("int32", "0") }),
            Replace(module, "begin", store, store with { OperatorKind = "raw" }),
            Replace(module, "resume", read, read with { ResultId = "erased" }),
            Replace(module, "resume", read, read with { ResultId = "number" }),
            Replace(module, "resume", read, read with { OperandIds = new[] { "callback" } }),
            Replace(module, "resume", read, read with { OperandIds = new[] { "token", "token" } }),
            Replace(module, "resume", read, read with { ResultId = null }),
            Replace(module, "begin", store, new("convert", "token", new[] { "state" }, null, null, null)),
        };
        foreach (string id in new[] { "store", "read" })
        {
            foreach (Func<GuestImport, GuestImport> mutate in new Func<GuestImport, GuestImport>[]
            {
                import => import with { Module = "env" }, import => import with { Name = "arbitrary_host" },
                import => import with { ParameterTypeIds = Array.Empty<string>() },
                import => import with { ReturnTypeId = V }, import => import with { BindingOrdinal = 0 },
                import => import with { OptimizationClass = "prepared" },
            }) invalid.Add(module with { Imports = module.Imports.Select(import => import.Id == id ? mutate(import) : import).ToArray() });
        }
        foreach (GuestModule candidate in invalid)
        {
            GuestValidationResult validation = GuestModuleValidator.Validate(candidate);
            Check(!validation.Succeeded && !WasmModuleCompiler.Compile(candidate).Succeeded, "invalid state contract reached emission");
            if (candidate.SchemaVersion == 11 && candidate.IrVersion == "1.10")
                Check(validation.Diagnostics.Any(item => item.Code == "ASIR1013"), "state rejection must include the managed ownership diagnostic");
        }
        // Existing modules remain accepted under every previously supported header.
        for (int version = 1; version <= 10; ++version)
            Compile(WasmModuleCompilerTests.CreateMinimalModule() with { SchemaVersion = version, IrVersion = $"1.{version - 1}" });
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_MANAGED_HEAP_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output, "continuation-state.wasm"), compiled.Bytes);
            File.WriteAllBytes(Path.Combine(output, "continuation-state-wrong.wasm"), wrong.Bytes);
            File.WriteAllBytes(Path.Combine(output, "continuation-state.guest-ir.json"), json);
        }
        DirectoryInfo? directory = new(AppContext.BaseDirectory);
        const string header = "Source/AvidScriptCore/Public/AvidScriptContinuationStateAbi.h";
        while (directory is not null && !File.Exists(Path.Combine(directory.FullName, header))) directory = directory.Parent;
        Check(directory is not null, "native state ABI header exists");
        string native = File.ReadAllText(Path.Combine(directory!.FullName, header));
        Check(native.Contains($"StoreImport[] = \"{GuestContinuationState.StoreImport}\"", StringComparison.Ordinal)
            && native.Contains($"ReadImport[] = \"{GuestContinuationState.ReadImport}\"", StringComparison.Ordinal), "state ABI identity drift");
        return invalid.Count + 17;
    }

    private static GuestModule Create()
    {
        GuestModule basis = WasmModuleCompilerTests.CreateMinimalModule();
        GuestType Ref(string id, string? payload) => new(id, "managed_ref", "i64", Array.Empty<GuestField>(), payload, null, 8, 8);
        GuestTypeLayoutResult types = GuestDataLayout.ComputeTypes(basis.Types.Concat(new[]
        {
            new GuestType(L, "scalar", "i64", Array.Empty<GuestField>(), null, null, 8, 8),
            new GuestType(F, "scalar", "f32", Array.Empty<GuestField>(), null, null, 4, 4),
            Ref(R, "type:node"), Ref(S, "type:state"), Ref(W, "type:state"), Ref(E, null),
            new GuestType("type:node", "struct", "memory", new[] { new GuestField("number", "number", I, 0), new GuestField("next", "next", R, 0) }, null, null, 0, 1),
            new GuestType("type:state", "struct", "memory", new[] { new GuestField("child", "child", R, 0) }, null, null, 0, 1),
        }).ToArray());
        Check(types.Succeeded, "state layout");
        GuestGlobal[] globals = { new("result", I, true, new("int32", "0")), new("accepted", I, true, new("int32", "0")), new("duplicate", I, true, new("int32", "-1")), new("token", L, true, new("int64", "0")) };
        GuestLayoutResult layout = GuestLayoutBuilder.Build(types.Types, globals, Array.Empty<GuestDataSegment>());
        Check(layout.Succeeded && layout.Layout!.StateSlots.Single(slot => slot.GlobalId == "accepted").Offset == 16
            && layout.Layout.StateSlots.Single(slot => slot.GlobalId == "duplicate").Offset == 20
            && layout.Layout.StateSlots.Single(slot => slot.GlobalId == "result").Offset == 24
            && layout.Layout.StateSlots.Single(slot => slot.GlobalId == "token").Offset == 32, "fixture state slots sorted by global identity");
        GuestFunction begin = Fn("begin", Array.Empty<GuestRegister>(), new[]
        { Reg("state", S), Reg("child", R), Reg("token", L), Reg("number", I), Reg("delay", F), Reg("accepted", I), Reg("duplicate", I), Reg("erased", E) }, new[]
        {
            Num("number", "int32", "42"), Num("delay", "float32", "0.01"),
            Op("call", "token", new[] { "delay", "number" }, "delay"),
            Op("global_store", null, new[] { "token" }, "token"),
            Op("managed_new", "child"), Op("managed_set", null, new[] { "child", "number" }, "number"),
            Op("managed_set", null, new[] { "child", "child" }, "next"),
            Op("managed_new", "state"), Op("managed_set", null, new[] { "state", "child" }, "child"),
            Op("managed_cast", "erased", new[] { "state" }),
            Op(GuestContinuationState.StoreOp, "accepted", new[] { "token", "state" }, "store"),
            Op("global_store", null, new[] { "accepted" }, "accepted"),
            Op(GuestContinuationState.StoreOp, "duplicate", new[] { "token", "state" }, "store"),
            Op("global_store", null, new[] { "duplicate" }, "duplicate"),
        });
        GuestFunction resume = Fn("resume", new[] { Reg("callback", I), Reg("token", L), Reg("status", I) },
            new[] { Reg("state", S), Reg("child", R), Reg("number", I), Reg("erased", E) }, new[]
        {
            Op(GuestContinuationState.ReadOp, "state", new[] { "token" }, "read"),
            Op("managed_collect"), Op("managed_get", "child", new[] { "state" }, "child"),
            Op("managed_collect"), Op("managed_get", "number", new[] { "child" }, "number"),
            Op("global_store", null, new[] { "number" }, "result"),
        });
        return basis with
        {
            SchemaVersion = 11, IrVersion = "1.10", Types = types.Types, Globals = globals, MemoryLayout = layout.Layout!,
            Imports = new[]
            {
                new GuestImport("heap", GuestManagedHeap.ImportModule, GuestManagedHeap.ImportName, new[] { I, I, I, I }, I),
                new GuestImport("store", GuestContinuationState.ImportModule, GuestContinuationState.StoreImport, new[] { L, I, L }, I),
                new GuestImport("read", GuestContinuationState.ImportModule, GuestContinuationState.ReadImport, new[] { L, I }, L),
                new GuestImport("delay", "env", "continuation_delay", new[] { F, I }, L),
            },
            Functions = new[] { begin, resume, Fn("tick", new[] { Reg("dt", F) }, Array.Empty<GuestRegister>(), Array.Empty<GuestInstruction>()) },
            Exports = new[] { new GuestExport("avid_on_begin_play", "begin"), new GuestExport("avid_on_continuation", "resume"), new GuestExport("avid_on_tick", "tick") },
        };
    }
    private static GuestModule Replace(GuestModule module, string id, GuestInstruction before, GuestInstruction after) => module with
    { Functions = module.Functions.Select(fn => fn.Id != id ? fn : fn with { Blocks = fn.Blocks.Select(block => block with { Instructions = block.Instructions.Select(op => ReferenceEquals(op, before) ? after : op).ToArray() }).ToArray() }).ToArray() };
    private static GuestRegister Reg(string id, string type) => new(id, type);
    private static GuestInstruction Op(string op, string? result = null, string[]? args = null, string? target = null) => new(op, result, args ?? Array.Empty<string>(), target, null, null);
    private static GuestInstruction Num(string result, string kind, string value) => new("constant", result, Array.Empty<string>(), null, null, new(kind, value));
    private static GuestFunction Fn(string id, GuestRegister[] parameters, GuestRegister[] locals, GuestInstruction[] ops) => new(id, parameters, locals, V, "entry", new[] { new GuestBasicBlock("entry", ops, new("return", null, null, null, null)) });
    private static WasmCompilationResult Compile(GuestModule module)
    {
        WasmCompilationResult result = WasmModuleCompiler.Compile(module);
        Check(result.Succeeded, string.Join(" | ", result.Diagnostics.Select(item => item.Message))); return result;
    }
    private static void Check(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }
}
