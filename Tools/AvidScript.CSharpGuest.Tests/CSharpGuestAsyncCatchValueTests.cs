using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Nodes;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestAsyncCatchValueTests
{
    public static int Run()
    {
        int count = 0;
        void Check(bool valid, string reason) { if (!valid) throw new InvalidOperationException(reason); count++; }
        string? directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_ASYNC_CATCH_FIXTURE_DIR");
        List<object> fixtures = new();
        foreach (var scenario in new[] {
            ("unused", "try { Sync(-1); } catch (ArgumentException error) { Trace++; } return 1;", 1, 1),
            ("value", "try { int value = await Read(1); Sync(-value); } catch (ArgumentException error) { return error == null ? 0 : SyncCatch(); } return 0;", 2, 0),
            ("escaped", "Exception saved = null; try { Sync(-1); } catch (ArgumentException error) { saved = error; } int value = await Read(3); return saved == null ? 0 : value;", 3, 0),
            ("rethrow", "Exception saved = null; try { try { Sync(-1); } catch (ArgumentException error) { saved = error; throw; } } catch (Exception again) { return saved == again ? 4 : 0; } return 0;", 4, 0),
            ("repeat-await", "Exception saved = null; Task<int> pending = Read(-1); try { int value = await pending; } catch (InvalidOperationException error) { saved = error; } try { int value = await pending; } catch (InvalidOperationException again) { return saved == again ? 5 : 0; } return 0;", 5, 0),
            ("cancel-escaped", "Exception saved = null; try { await AvidContinuations.NextTickAsync(); } catch (OperationCanceledException error) { saved = error; } await AvidContinuations.NextTickAsync(); return saved == null ? 0 : 6;", 6, 0),
        })
        foreach (bool deferred in new[] { false, true })
        {
            string name = scenario.Item1 + (deferred ? "-deferred" : "-ready");
            bool cancel = scenario.Item1 == "cancel-escaped";
            string source = Source(scenario.Item2, deferred);
            Check(CSharpGuestAsyncThrowRoutingTests.Reference(source, cancel) == (scenario.Item3, scenario.Item4), name + " .NET result and trace");
            string sourceId = "Scripts/AsyncCatch_" + name + ".cs";
            var semantic = SemanticAnalyzer.Analyze(source, sourceId, FrontendAnalyzer.Analyze(source, sourceId).Source.Sha256,
                new[] { new SemanticReferenceSource(CSharpGuestContinuationTests.ReferenceFacade + CSharpGuestAsyncThrowRoutingTests.CancelFacade,
                    "generated://Continuations.cs", true) }, new SemanticCompilerWorkspace(),
                enableAsyncExceptionFlow: true, enableDirectAwaitCleanup: true, enableAsyncCancellationFlow: true,
                enableAsyncSynchronousExceptions: true, enableAsyncCatchVariables: true);
            Check(SemanticContract.HasAsyncCatchVariables(semantic) && SemanticAsyncCatchVariableValidator.IsValid(semantic),
                name + " source bindings: " + string.Join(" | ", semantic.Diagnostics.Select(d => d.Message)));
            string hash = Convert.ToHexString(SHA256.HashData(SemanticSerializer.Serialize(semantic))).ToLowerInvariant();
            Check(CSharpLanguageErrorCompiler.TryLower(semantic, hash, out var compilation, out var error)
                && compilation is not null, name + " lowering: " + error);
            var module = compilation!.Module;
            Check(GuestExceptionValues.IsVersion(module) && module.ExceptionValues?.Bindings.Count > 0
                && module.Provenance.SemanticSchemaVersion == 52 && module.Provenance.SemanticVersion == "1.61",
                name + " source and execution identity");
            var wasm = WasmModuleCompiler.Compile(module);
            Check(wasm.Succeeded, name + " WASM: " + string.Join(" | ", wasm.Diagnostics.Select(d => d.Message)));
            byte[] json = GuestIrSerializer.Serialize(module);
            Check(json.SequenceEqual(GuestIrSerializer.Serialize(GuestIrSerializer.Deserialize(json)))
                && wasm.Bytes.SequenceEqual(WasmModuleCompiler.Compile(GuestIrSerializer.Deserialize(json)).Bytes), name + " canonical round trip");
            var provenance = WasmArtifactInspector.Inspect(wasm.Bytes).CustomSections.Single(section => section.Name == "avidscript.provenance").PayloadText;
            Check(provenance.Split('\n').Contains("guest_ir=33/1.32") && provenance.Split('\n').Contains("guest_ir_base=29/1.28")
                && provenance.Split('\n').Contains("semantic=52/1.61"), name + " WASM source and execution contract");
            if (name == "value-deferred") CheckRejectedMutations(module, Check);
            CheckTokenComposition(module, Check);
            if (string.IsNullOrWhiteSpace(directory)) continue;
            Directory.CreateDirectory(directory);
            File.WriteAllText(Path.Combine(directory, name + ".cs"), source);
            File.WriteAllBytes(Path.Combine(directory, name + ".semantic.json"), SemanticSerializer.Serialize(semantic));
            File.WriteAllBytes(Path.Combine(directory, name + ".guest-ir.json"), json);
            File.WriteAllBytes(Path.Combine(directory, name + ".wasm"), wasm.Bytes);
            fixtures.Add(new { name, moduleId = module.ModuleId, cancel, expected = scenario.Item3, trace = scenario.Item4,
                resultOffset = module.MemoryLayout.StateSlots.Single(slot => slot.GlobalId.Contains(".Result:", StringComparison.Ordinal)).Offset,
                traceOffset = module.MemoryLayout.StateSlots.Single(slot => slot.GlobalId.Contains(".Trace:", StringComparison.Ordinal)).Offset });
        }
        CheckUnnamedCatchTokenComposition(Check);
        if (!string.IsNullOrWhiteSpace(directory))
            File.WriteAllText(Path.Combine(directory, "cases.json"), JsonSerializer.Serialize(fixtures, new JsonSerializerOptions { WriteIndented = true }));
        return count;
    }

    // A real compiled async/catch module supplies the ownership graph. This
    // composition check does not claim that Semantic 53 lowering is available.
    private static void CheckTokenComposition(GuestModule original, Action<bool, string> check)
    {
        var reader = new GuestFunction("token:read", new[] { new GuestRegister("root", GuestCancellationTokens.RootTypeId) },
            new[] { new GuestRegister("identity", "type:int64") }, "type:int64", "entry", new[] {
                new GuestBasicBlock("entry", new[] { new GuestInstruction("call", "identity", new[] { "root" },
                    GuestCancellationTokens.ReadImportId, null, null) }, new("return", null, null, null, "identity")),
            });
        var module = original with {
            SchemaVersion = 34, IrVersion = "1.33", CancellationTokens = new(29, "1.28"),
            Provenance = original.Provenance with { SemanticSchemaVersion = 53, SemanticVersion = "1.62" },
            Types = original.Types.Append(GuestCancellationTokens.ValueType()).ToArray(),
            Imports = original.Imports.Append(GuestCancellationTokens.Reader()).ToArray(),
            Functions = original.Functions.Append(reader).ToArray(),
        };
        var valid = GuestModuleValidator.Validate(module);
        check(valid.Succeeded, "IR34 async composition: " + string.Join(" | ", valid.Diagnostics.Select(item => item.Code + ": " + item.Message)));
        var wasm = WasmModuleCompiler.Compile(module);
        check(wasm.Succeeded, "IR34 async WASM: " + string.Join(" | ", wasm.Diagnostics.Select(item => item.Message)));
        var json = GuestIrSerializer.Serialize(module);
        check(json.SequenceEqual(GuestIrSerializer.Serialize(GuestIrSerializer.Deserialize(json)))
            && wasm.Bytes.SequenceEqual(WasmModuleCompiler.Compile(GuestIrSerializer.Deserialize(json)).Bytes), "IR34 async deterministic round trip");
        var provenance = WasmArtifactInspector.Inspect(wasm.Bytes).CustomSections.Single(item => item.Name == "avidscript.provenance").PayloadText.Split('\n');
        check(provenance.Contains("guest_ir=34/1.33") && provenance.Contains("guest_ir_base=29/1.28")
            && provenance.Contains("semantic=53/1.62"), "IR34 async provenance");
        void Reject(GuestModule invalid, string reason) => check(!GuestModuleValidator.Validate(invalid).Succeeded
            && !WasmModuleCompiler.Compile(invalid).Succeeded, "IR34 async composition accepted " + reason);
        Reject(module with { CancellationIdentity = null }, "missing cancellation identity");
        if (module.ExceptionValues is not null)
            Reject(module with { ExceptionValues = null }, "missing exception bindings");
        else
            check(GuestIrSerializer.Deserialize(json).ExceptionValues is null, "Token values must not invent named catch bindings");
        Reject(module with { AsyncSynchronousExceptions = null }, "missing synchronous error plan");
        Reject(module with { TaskLocalLifetimes = null }, "missing Task ownership");
        Reject(module with { CancellationTokens = new(17, "1.16") }, "downgraded execution base");
        Reject(module with { CancellationIdentity = module.CancellationIdentity! with { BaseSchemaVersion = 25, BaseIrVersion = "1.24" } }, "mismatched identity base");
        if (module.ExceptionValues is not { } bindings) return;
        var binding = bindings.Bindings[0];
        Reject(module with { ExceptionValues = new(module.ExceptionValues.Bindings.Select(item => item == binding
            ? item with { OwnerLocalId = binding.VariableLocalId } : item).ToArray()) }, "forged catch owner");
        Reject(module with { Functions = module.Functions.Select(function => function.Id == binding.FunctionId ? function with {
            Blocks = function.Blocks.Select(block => block.Id == binding.BlockId ? block with {
                Instructions = block.Instructions.Skip(1).ToArray() } : block).ToArray() } : function).ToArray() }, "missing root acquisition");
    }

    private static void CheckUnnamedCatchTokenComposition(Action<bool, string> check)
    {
        string source = Source("try { await AvidContinuations.NextTickAsync(); } catch (OperationCanceledException) { return 6; } return 1;", true);
        const string sourceId = "Scripts/TokenUnnamedCatch.cs";
        var semantic = SemanticAnalyzer.Analyze(source, sourceId, FrontendAnalyzer.Analyze(source, sourceId).Source.Sha256,
            new[] { new SemanticReferenceSource(CSharpGuestContinuationTests.ReferenceFacade + CSharpGuestAsyncThrowRoutingTests.CancelFacade,
                "generated://Continuations.cs", true) }, new SemanticCompilerWorkspace(),
            enableAsyncExceptionFlow: true, enableDirectAwaitCleanup: true, enableAsyncCancellationFlow: true,
            enableAsyncSynchronousExceptions: true);
        string hash = Convert.ToHexString(SHA256.HashData(SemanticSerializer.Serialize(semantic))).ToLowerInvariant();
        check(CSharpLanguageErrorCompiler.TryLower(semantic, hash, out var compiled, out var error)
            && compiled is not null, "Unnamed catch source lowering: " + error);
        check(compiled!.Module is { ExceptionValues: null, SchemaVersion: 29 }, "Unnamed catch uses the existing IR29 execution base");
        check(CSharpCancellationIdentityCompiler.TryUpgrade(compiled.Module, out var identity, out error)
            && identity is not null, "Unnamed catch identity upgrade: " + error);
        CheckTokenComposition(identity!, check);
    }

    private static void CheckRejectedMutations(GuestModule module, Action<bool, string> check)
    {
        var plan = module.ExceptionValues!;
        var binding = plan.Bindings[0];
        var function = module.Functions.Single(item => item.Id == binding.FunctionId);
        void Reject(GuestModule invalid, string reason) => check(!GuestModuleValidator.Validate(invalid).Succeeded
            && !WasmModuleCompiler.Compile(invalid).Succeeded, "Catch values reject " + reason);
        GuestModule Change(GuestExceptionValueBinding value) => module with {
            ExceptionValues = new(plan.Bindings.Select(item => item == binding ? value : item).ToArray()) };
        GuestModule Rewrite(string id, Func<GuestBasicBlock, GuestBasicBlock> update) => module with {
            Functions = module.Functions.Select(item => item != function ? item : item with {
                Blocks = item.Blocks.Select(block => block.Id == id ? update(block) : block).ToArray() }).ToArray() };
        Reject(module with { ExceptionValues = null }, "missing bindings");
        Reject(module with { ExceptionValues = new(Array.Empty<GuestExceptionValueBinding>()) }, "empty bindings");
        Reject(module with { ExceptionValues = new(null!) }, "null bindings");
        Reject(module with { ExceptionValues = new(new GuestExceptionValueBinding[] { null! }) }, "null binding");
        Reject(module with { ExceptionValues = new(plan.Bindings.Skip(1).ToArray()) }, "unlisted capture");
        Reject(module with { ExceptionValues = new(plan.Bindings.Append(binding).ToArray()) }, "duplicate binding");
        Reject(module with { SchemaVersion = 32, IrVersion = "1.31" }, "downgraded IR");
        Reject(module with { SchemaVersion = 34, IrVersion = "1.33" }, "future IR");
        Reject(module with { IrVersion = "1.31" }, "mismatched IR");
        Reject(module with { Provenance = module.Provenance with { SemanticSchemaVersion = 50, SemanticVersion = "1.59" } }, "old source identity");
        Reject(module with { CancellationIdentity = null }, "missing cancellation ownership");
        Reject(Change(binding with { FunctionId = null! }), "null function");
        Reject(Change(binding with { OwnerLocalId = binding.VariableLocalId }), "substituted owner");
        Reject(Change(binding with { ReferenceTypeId = "type:int64" }), "unmanaged exception view");
        Reject(Rewrite(binding.BlockId, block => block with { Instructions = block.Instructions.Skip(1).ToArray() }), "missing owner acquisition");
        Reject(Rewrite(binding.BlockId, block => block with { Instructions = block.Instructions.Select((instruction, index) => index == 1
            ? instruction with { OperandIds = new[] { binding.OwnerLocalId } } : instruction).ToArray() }), "bypassed owner load");
        Reject(Rewrite(binding.BlockId, block => block with { Instructions = block.Instructions.Select((instruction, index) => index == 2
            ? instruction with { OperandIds = new[] { binding.VariableLocalId } } : instruction).ToArray() }), "wrong object identity");
        Reject(module with { Functions = module.Functions.Select(item => item != function ? item
            : item with { EntryBlockId = binding.BlockId }).ToArray() }, "entry bypassing catch match");
        var predecessor = function.Blocks.First(block => block.Terminator.TargetBlockId == binding.BlockId);
        Reject(Rewrite(predecessor.Id, block => block with { Terminator = block.Terminator with {
            TargetBlockId = block.Terminator.FalseTargetBlockId, FalseTargetBlockId = binding.BlockId } }), "failed catch match entering binding");
        foreach (string member in new[] { "owner_local_id", "reference_type_id", "future_field" })
        {
            var document = JsonNode.Parse(GuestIrSerializer.Serialize(module))!.AsObject();
            var entry = document["exception_values"]!["bindings"]![0]!.AsObject();
            if (member == "future_field") entry[member] = 1; else entry.Remove(member);
            bool rejected = false;
            try { GuestIrSerializer.Deserialize(JsonSerializer.SerializeToUtf8Bytes(document)); }
            catch (InvalidDataException) { rejected = true; }
            check(rejected, "Catch binding serializer rejects " + member);
        }
    }

    private static string Source(string body, bool deferred) =>
        "using AvidScript; using System; using System.Runtime.InteropServices; using System.Threading.Tasks; public static class Script { "
        + "public static int Result; public static int Trace; "
        + "[UnmanagedCallersOnly(EntryPoint = \"avid_on_begin_play\")] public static async void BeginPlay() { Result = await Run(); } "
        + "public static int Sync(int value) { if (value < 0) throw new ArgumentException(); return value; } "
        + (body.Contains("SyncCatch()", StringComparison.Ordinal)
            ? "public static int SyncCatch() { try { Sync(-1); return 0; } catch (Exception error) { return error == null ? 0 : 2; } } " : "")
        + "public static async Task<int> Read(int value) { " + (deferred ? "await AvidContinuations.NextTickAsync(); " : "")
        + "if (value < 0) throw new InvalidOperationException(); return value; } "
        + "public static async Task<int> Run() { " + body + " } }";
}
