using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class WasmEventStateTests
{
    private const string I = "type:int32", L = "type:int64", F = "type:float32", V = "type:void";
    private const string R = "type:node_ref", S = "type:state_ref", W = "type:wrong_ref", E = "type:erased";

    public static int Run()
    {
        GuestModule module = Create();
        byte[] json = GuestIrSerializer.Serialize(module);
        Check(json.SequenceEqual(GuestIrSerializer.Serialize(GuestIrSerializer.Deserialize(json))), "event IR round trip");
        WasmCompilationResult compiled = Compile(module);
        Check(compiled.Bytes.SequenceEqual(Compile(module).Bytes), "event codegen determinism");
        Check(WasmArtifactInspector.Inspect(compiled.Bytes).Imports.Count == 6, "event emission adds no hidden imports");
        GuestInstruction subscribe = module.Functions[1].Blocks[0].Instructions.Single(op => op.Op == GuestEventState.SubscribeOp);
        GuestInstruction read = module.Functions[2].Blocks[0].Instructions[0];
        List<GuestModule> invalid = new()
        {
            module with { SchemaVersion = 11, IrVersion = "1.10" },
            module with { SchemaVersion = GuestModuleValidator.CurrentSchemaVersion + 1, IrVersion = "future" },
            module with { IrVersion = "1.10" },
            module with { Imports = module.Imports.Where(import => import.Id != "subscribe").ToArray() },
            module with { Imports = module.Imports.Where(import => import.Id != "heap").ToArray() },
            Replace(module, "tick", subscribe, subscribe with { TargetId = "read" }),
            Replace(module, "tick", subscribe, subscribe with { TargetId = null }),
            Replace(module, "tick", subscribe, subscribe with { ResultId = null }),
            Replace(module, "tick", subscribe, subscribe with { ResultId = "number" }),
            Replace(module, "tick", subscribe, subscribe with { OperandIds = Array.Empty<string>() }),
            Replace(module, "tick", subscribe, subscribe with { OperandIds = new[] { "slot", "generation", "ordinal", "state", "state" } }),
            Replace(module, "tick", subscribe, subscribe with { OperandIds = new[] { "slot", "generation", "ordinal", "token" } }),
            Replace(module, "tick", subscribe, subscribe with { OperandIds = new[] { "slot", "generation", "ordinal", "erased" } }),
            Replace(module, "tick", subscribe, subscribe with { OperandIds = new[] { "token", "generation", "ordinal", "state" } }),
            Replace(module, "tick", subscribe, subscribe with { OperandIds = new[] { "slot", "token", "ordinal", "state" } }),
            Replace(module, "tick", subscribe, subscribe with { OperandIds = new[] { "slot", "generation", "token", "state" } }),
            Replace(module, "tick", subscribe, subscribe with { Constant = new("int32", "0") }),
            Replace(module, "tick", subscribe, subscribe with { OperatorKind = "raw" }),
            Replace(module, "event", read, read with { ResultId = "erased" }),
            Replace(module, "event", read, read with { ResultId = "token" }),
            Replace(module, "event", read, read with { OperandIds = new[] { "number" } }),
            Replace(module, "event", read, read with { ResultId = null }),
            Replace(module, "event", read, read with { TargetId = null }),
            Replace(module, "event", read, read with { Constant = new("int32", "1") }),
            Replace(module, "event", read, read with { OperatorKind = "raw" }),
            Replace(module, "tick", subscribe, Op("convert", "token", new[] { "state" })),
        };
        foreach (string id in new[] { "subscribe", "read" })
        {
            foreach (Func<GuestImport, GuestImport> mutate in new Func<GuestImport, GuestImport>[]
            {
                import => import with { Module = "env" }, import => import with { Name = "same_signature_host" },
                import => import with { ParameterTypeIds = Array.Empty<string>() },
                import => import with { ParameterTypeIds = import.ParameterTypeIds.Select((type, index) => index == 0 ? L : type).ToArray() },
                import => import with { ParameterTypeIds = import.ParameterTypeIds.Select((type, index) => index == import.ParameterTypeIds.Count - 1 ? F : type).ToArray() },
                import => import with { ReturnTypeId = I }, import => import with { BindingOrdinal = 0 },
                import => import with { OptimizationClass = "prepared" },
            }) invalid.Add(module with { Imports = module.Imports.Select(import => import.Id == id ? mutate(import) : import).ToArray() });
        }
        foreach (GuestModule candidate in invalid)
        {
            GuestValidationResult validation = GuestModuleValidator.Validate(candidate);
            Check(!validation.Succeeded && !WasmModuleCompiler.Compile(candidate).Succeeded, "invalid event state reached codegen");
            if (candidate.SchemaVersion == 12 && candidate.IrVersion == "1.11")
                Check(validation.Diagnostics.Any(item => item.Code == "ASIR1013"), "invalid event state lacks ownership diagnostic");
        }
        for (int version = 1; version <= 11; ++version)
            Compile(WasmModuleCompilerTests.CreateMinimalModule() with { SchemaVersion = version, IrVersion = $"1.{version - 1}" });
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_MANAGED_HEAP_WASM_DIR");
        foreach (string variant in new[] { "normal", "self-cancel", "wrong", "outside" })
        {
            WasmCompilationResult artifact = Compile(Create(variant));
            if (!string.IsNullOrWhiteSpace(output))
            {
                Directory.CreateDirectory(output);
                File.WriteAllBytes(Path.Combine(output, $"event-state-{variant}.wasm"), artifact.Bytes);
            }
        }
        if (!string.IsNullOrWhiteSpace(output)) File.WriteAllBytes(Path.Combine(output, "event-state.guest-ir.json"), json);
        DirectoryInfo? directory = new(AppContext.BaseDirectory);
        const string header = "Source/AvidScriptCore/Public/AvidScriptEventStateAbi.h";
        while (directory is not null && !File.Exists(Path.Combine(directory.FullName, header))) directory = directory.Parent;
        Check(directory is not null, "native event ABI header exists");
        string native = File.ReadAllText(Path.Combine(directory!.FullName, header));
        Check(native.Contains($"SubscribeImport[] = \"{GuestEventState.SubscribeImport}\"", StringComparison.Ordinal)
            && native.Contains($"ReadImport[] = \"{GuestEventState.ReadImport}\"", StringComparison.Ordinal)
            && native.Contains($"LanguageSubscribeImport[] = \"{GuestEventState.LanguageSubscribeImport}\"", StringComparison.Ordinal)
            && native.Contains($"LanguageLookupImport[] = \"{GuestEventState.LanguageLookupImport}\"", StringComparison.Ordinal), "event ABI name drift");
        GuestModule language = CreateLanguage();
        WasmCompilationResult languageArtifact = Compile(language);
        Check(WasmArtifactInspector.Inspect(languageArtifact.Bytes).Imports.Count == 8, "language IR emits declared imports only");
        if (!string.IsNullOrWhiteSpace(output)) File.WriteAllBytes(Path.Combine(output, "event-language-state.wasm"), languageArtifact.Bytes);
        GuestInstruction languageSubscribe = language.Functions[1].Blocks[0].Instructions.Single(op => op.Op == GuestEventState.LanguageSubscribeOp);
        GuestInstruction languageLookup = language.Functions[1].Blocks[0].Instructions.First(op => op.Op == GuestEventState.LanguageLookupOp);
        GuestModule[] badLanguage =
        {
            language with { SchemaVersion = 13, IrVersion = "1.12" },
            Replace(language, "tick", languageSubscribe, languageSubscribe with { TargetId = "subscribe" }),
            Replace(language, "tick", languageSubscribe, languageSubscribe with { OperandIds = new[] { "slot", "generation", "ordinal", "erased" } }),
            Replace(language, "tick", languageLookup, languageLookup with { TargetId = "read" }),
            Replace(language, "tick", languageLookup, languageLookup with { OperandIds = new[] { "slot", "generation" } }),
            Replace(language, "tick", languageLookup, languageLookup with { ResultId = "erased" }),
            language with { Imports = language.Imports.Where(import => import.Id != "language_lookup").ToArray() },
        };
        foreach (GuestModule bad in badLanguage)
            Check(!GuestModuleValidator.Validate(bad).Succeeded, "language IR rejects malformed or legacy capabilities");
        return invalid.Count + badLanguage.Length + 21;
    }

    private static GuestModule Create(string variant = "normal")
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
        Check(types.Succeeded, "event state layouts");
        GuestGlobal[] globals = { new("count", I, true, new("int32", "0")), new("result", I, true, new("int32", "0")), new("subscription", L, true, new("int64", "0")) };
        GuestLayoutResult layout = GuestLayoutBuilder.Build(types.Types, globals, Array.Empty<GuestDataSegment>());
        Check(layout.Succeeded && layout.Layout!.StateSlots.Single(slot => slot.GlobalId == "count").Offset == 16
            && layout.Layout.StateSlots.Single(slot => slot.GlobalId == "result").Offset == 20
            && layout.Layout.StateSlots.Single(slot => slot.GlobalId == "subscription").Offset == 24, "event fixture state offsets");
        GuestRegister[] locals = { Reg("state", S), Reg("child", R), Reg("number", I), Reg("slot", I), Reg("generation", I), Reg("ordinal", I), Reg("token", L), Reg("erased", E) };
        GuestFunction tick = Fn("tick", new[] { Reg("dt", F) }, locals, variant == "outside"
            ? new[] { Op(GuestEventState.ReadOp, "state", target: "read") }
            : new[]
        {
            Op("call", "slot", target: "slot"), Op("call", "generation", target: "generation"), Num("ordinal", "7"), Num("number", "42"),
            Op("managed_new", "child"), Op("managed_set", args: new[] { "child", "number" }, target: "number"),
            Op("managed_set", args: new[] { "child", "child" }, target: "next"),
            Op("managed_new", "state"), Op("managed_set", args: new[] { "state", "child" }, target: "child"),
            Op(GuestEventState.SubscribeOp, "token", new[] { "slot", "generation", "ordinal", "state" }, "subscribe"),
            Op("global_store", args: new[] { "token" }, target: "subscription"), Op("managed_collect"),
        });
        List<GuestInstruction> callback = new();
        if (variant == "self-cancel")
        {
            callback.Add(Op("global_load", "token", target: "subscription"));
            callback.Add(Op("call", "cancelled", new[] { "token" }, "unsubscribe"));
        }
        callback.AddRange(new[]
        {
            Op(GuestEventState.ReadOp, "state", target: "read"), Op("managed_collect"),
            Op("managed_get", "child", new[] { "state" }, "child"), Op("managed_collect"),
            Op("managed_get", "number", new[] { "child" }, "number"), Num("one", "1"),
            new GuestInstruction("binary", "updated", new[] { "number", "one" }, null, "add", null),
            Op("managed_set", args: new[] { "child", "updated" }, target: "number"),
            Op("global_store", args: new[] { "updated" }, target: "result"),
            Op("global_load", "count", target: "count"),
            new GuestInstruction("binary", "nextCount", new[] { "count", "one" }, null, "add", null),
            Op("global_store", args: new[] { "nextCount" }, target: "count"),
        });
        return basis with
        {
            SchemaVersion = 12, IrVersion = "1.11", Types = types.Types, Globals = globals, MemoryLayout = layout.Layout!,
            Imports = new[]
            {
                new GuestImport("heap", GuestManagedHeap.ImportModule, GuestManagedHeap.ImportName, new[] { I, I, I, I }, I),
                new GuestImport("subscribe", GuestEventState.ImportModule, GuestEventState.SubscribeImport, new[] { I, I, I, I, L }, L),
                new GuestImport("read", GuestEventState.ImportModule, GuestEventState.ReadImport, new[] { I }, L),
                new GuestImport("unsubscribe", "avidscript", "event_unsubscribe", new[] { L }, I),
                new GuestImport("slot", "avidscript", "owner_get_slot", Array.Empty<string>(), I),
                new GuestImport("generation", "avidscript", "owner_get_generation", Array.Empty<string>(), I),
            },
            Functions = new[]
            {
                Fn("begin", Array.Empty<GuestRegister>(), Array.Empty<GuestRegister>(), Array.Empty<GuestInstruction>()), tick,
                Fn("event", new[] { Reg("dt", F) }, new[] { Reg("state", variant == "wrong" ? W : S), Reg("child", R), Reg("number", I), Reg("one", I), Reg("token", L), Reg("erased", E), Reg("updated", I), Reg("count", I), Reg("nextCount", I), Reg("cancelled", I) }, callback.ToArray()),
            },
            Exports = new[] { new GuestExport("avid_on_begin_play", "begin"), new GuestExport("avid_on_tick", "tick"), new GuestExport("event_callback", "event") },
        };
    }
    private static GuestModule CreateLanguage()
    {
        GuestModule module = Create();
        GuestFunction tick = module.Functions[1];
        GuestBasicBlock entry = tick.Blocks[0];
        GuestInstruction[] extra =
        {
            Op(GuestEventState.LanguageLookupOp, "languageState", new[] { "slot", "generation", "ordinal" }, "language_lookup"),
            Op(GuestEventState.LanguageSubscribeOp, "languageToken", new[] { "slot", "generation", "ordinal", "state" }, "language_subscribe"),
            Op("global_store", args: new[] { "languageToken" }, target: "subscription"),
            Op(GuestEventState.LanguageLookupOp, "languageFound", new[] { "slot", "generation", "ordinal" }, "language_lookup"),
            Op("managed_collect"),
            Op("managed_get", "languageChild", new[] { "languageFound" }, "child"),
            Op("managed_get", "languageNumber", new[] { "languageChild" }, "number"),
            Op("global_store", args: new[] { "languageNumber" }, target: "result"),
        };
        return module with
        {
            SchemaVersion = 14, IrVersion = "1.13",
            Imports = module.Imports.Concat(new[]
            {
                new GuestImport("language_subscribe", GuestEventState.ImportModule, GuestEventState.LanguageSubscribeImport, new[] { I, I, I, I, L }, L),
                new GuestImport("language_lookup", GuestEventState.ImportModule, GuestEventState.LanguageLookupImport, new[] { I, I, I, I }, L),
            }).ToArray(),
            Functions = module.Functions.Select(function => function.Id != "tick" ? function : function with
            {
                Locals = function.Locals.Concat(new[] { Reg("languageState", S), Reg("languageToken", L),
                    Reg("languageFound", S), Reg("languageChild", R), Reg("languageNumber", I) }).ToArray(),
                Blocks = new[] { entry with { Instructions = entry.Instructions.Concat(extra).ToArray() } },
            }).ToArray(),
        };
    }
    private static GuestModule Replace(GuestModule module, string id, GuestInstruction before, GuestInstruction after) => module with
    { Functions = module.Functions.Select(fn => fn.Id != id ? fn : fn with { Blocks = fn.Blocks.Select(block => block with { Instructions = block.Instructions.Select(op => ReferenceEquals(op, before) ? after : op).ToArray() }).ToArray() }).ToArray() };
    private static GuestRegister Reg(string id, string type) => new(id, type);
    private static GuestInstruction Op(string op, string? result = null, string[]? args = null, string? target = null) => new(op, result, args ?? Array.Empty<string>(), target, null, null);
    private static GuestInstruction Num(string result, string value) => new("constant", result, Array.Empty<string>(), null, null, new("int32", value));
    private static GuestFunction Fn(string id, GuestRegister[] parameters, GuestRegister[] locals, GuestInstruction[] ops) => new(id, parameters, locals, V, "entry", new[] { new GuestBasicBlock("entry", ops, new("return", null, null, null, null)) });
    private static WasmCompilationResult Compile(GuestModule module)
    {
        GuestValidationResult validation = GuestModuleValidator.Validate(module);
        Check(validation.Succeeded, string.Join(" | ", validation.Diagnostics.Select(item => item.Message)));
        WasmCompilationResult result = WasmModuleCompiler.Compile(module);
        Check(result.Succeeded, string.Join(" | ", result.Diagnostics.Select(item => item.Message))); return result;
    }
    private static void Check(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }
}
