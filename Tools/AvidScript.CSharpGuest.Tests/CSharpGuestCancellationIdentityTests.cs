using System;
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

internal static class CSharpGuestCancellationIdentityTests
{
    internal static int CheckUpgrade(GuestModule original, out GuestModule upgraded)
    {
        int count = 0;
        void Check(bool valid, string reason) { if (!valid) throw new InvalidOperationException(reason); count++; }
        byte[] oldJson = GuestIrSerializer.Serialize(original);
        byte[] oldWasm = WasmModuleCompiler.Compile(original).Bytes;
        Check(CSharpCancellationIdentityCompiler.TryUpgrade(original, out var result, out string? error)
            && result is not null, "Cancellation identity upgrade: " + error);
        var module = result!;
        upgraded = module;
        var profile = GuestDirectAwaitReadiness.BaseProfile(original);
        Check(module is { SchemaVersion: 32, IrVersion: "1.31", CancellationIdentity: not null }
            && module.CancellationIdentity.BaseSchemaVersion == profile.SchemaVersion
            && module.CancellationIdentity.BaseIrVersion == profile.IrVersion
            && module.Provenance == original.Provenance, "Identity envelope preserves the source execution profile");
        Check(GuestModuleValidator.Validate(module).Succeeded, "Independent identity validation");
        byte[] json = GuestIrSerializer.Serialize(module);
        Check(json.SequenceEqual(GuestIrSerializer.Serialize(GuestIrSerializer.Deserialize(json))), "Canonical identity round trip");
        Check(CSharpCancellationIdentityCompiler.TryUpgrade(original, out var again, out error)
            && json.SequenceEqual(GuestIrSerializer.Serialize(again!)), "Deterministic identity upgrade: " + error);
        Check(CSharpCancellationIdentityCompiler.TryUpgrade(module, out again, out error)
            && json.SequenceEqual(GuestIrSerializer.Serialize(again!)), "Idempotent validated identity upgrade: " + error);
        var wasm = WasmModuleCompiler.Compile(module);
        Check(wasm.Succeeded, "Identity codegen: " + string.Join(" | ", wasm.Diagnostics.Select(d => d.Message)));
        Check(wasm.Bytes.SequenceEqual(WasmModuleCompiler.Compile(GuestIrSerializer.Deserialize(json)).Bytes),
            "Deterministic identity WASM after serialization");
        var inspection = WasmArtifactInspector.Inspect(wasm.Bytes);
        Check(inspection.Imports.Any(import => import.Module == "avidscript" && import.Name == GuestTaskCancellationIdentity.CancelImportName)
            && inspection.Imports.Any(import => import.Module == "avidscript" && import.Name == GuestTaskCancellationIdentity.ReadImportName)
            && inspection.Imports.All(import => import.Name != GuestTaskCancellationErrorValidator.ImportName),
            "Emitted WASM uses only the versioned identity transport imports");
        string provenance = inspection.CustomSections.Single(section => section.Name == "avidscript.provenance").PayloadText;
        Check(provenance.Split('\n').Contains("guest_ir=32/1.31")
            && provenance.Split('\n').Contains($"guest_ir_base={profile.SchemaVersion}/{profile.IrVersion}"),
            "WASM carries the outer version and original execution profile");
        var catalogSection = inspection.CustomSections.Single(section => section.Name == "avidscript.language_errors");
        using var catalog = JsonDocument.Parse(catalogSection.PayloadText);
        Check(catalog.RootElement.GetProperty("guest_ir_schema_version").GetInt32() == 32
            && catalog.RootElement.GetProperty("guest_ir_version").GetString() == "1.31", "Catalog matches the identity envelope");
        Check(oldJson.SequenceEqual(GuestIrSerializer.Serialize(original))
            && oldWasm.SequenceEqual(WasmModuleCompiler.Compile(original).Bytes), "Upgrade leaves the legacy artifact unchanged");
        return count;
    }

    public static int Run()
    {
        int count = 0;
        void Check(bool valid, string reason) { if (!valid) throw new InvalidOperationException(reason); count++; }
        const string sourceId = "Fixtures/Phase66/AsyncCancellationFlow.cs";
        string source = File.ReadAllText(sourceId) + "\n" + File.ReadAllText("Fixtures/Phase66/AsyncCancellationFlow.Guest.cs");
        var semantic = SemanticAnalyzer.Analyze(source, sourceId, FrontendAnalyzer.Analyze(source, sourceId).Source.Sha256,
            new[] { new SemanticReferenceSource(CSharpGuestContinuationTests.ReferenceFacade + CSharpGuestAsyncThrowRoutingTests.CancelFacade,
                "generated://Continuations.cs", true) }, new SemanticCompilerWorkspace(),
            enableAsyncExceptionFlow: true, enableDirectAwaitCleanup: true, enableAsyncCancellationFlow: true);
        string hash = Convert.ToHexString(SHA256.HashData(SemanticSerializer.Serialize(semantic))).ToLowerInvariant();
        var lowered = CSharpGuestLowerer.Lower(semantic, hash, enableAsyncLanguageErrors: true);
        Check(lowered.Succeeded && lowered.Module is { SchemaVersion: 31, CancellationIdentity: null },
            "Default cancellation lowering stays on its legacy contract: " + string.Join(" | ", lowered.Diagnostics.Select(d => d.Message)));
        count += CheckUpgrade(lowered.Module!, out var module);
        void Reject(GuestModule invalid, string reason, string? code = "ASIR1039")
        {
            var validation = GuestModuleValidator.Validate(invalid);
            Check(!validation.Succeeded && (code is null || validation.Diagnostics.Any(d => d.Code == code))
                && !WasmModuleCompiler.Compile(invalid).Succeeded
                && !CSharpCancellationIdentityCompiler.TryUpgrade(invalid, out var rejected, out _) && rejected is null,
                "Identity must reject " + reason + ": " + string.Join(" | ", validation.Diagnostics.Select(d => d.Code + ": " + d.Message)));
        }
        Reject(module with { CancellationIdentity = null }, "missing envelope");
        Reject(module with { CancellationIdentity = new(24, "1.24") }, "mismatched base version");
        Reject(module with { CancellationIdentity = new(31, "1.30") }, "nested readiness envelope as execution profile");
        Reject(module with { CancellationIdentity = new(24, null!) }, "null required base version", "ASIR1001");
        Reject(module with { LanguageErrorCatalog = null }, "missing catalog");
        Reject(module with { DirectAwaitRoutes = null }, "missing producer routes", null);
        Reject(module with { DirectAwaitReadiness = null }, "missing readiness plan", "ASIR1038");
        Reject(module with { SchemaVersion = 31, IrVersion = "1.30" }, "downgraded envelope");
        Reject(module with { SchemaVersion = 33, IrVersion = "1.32" }, "future envelope");
        Reject(module with { IrVersion = "1.30" }, "mismatched envelope version");
        Reject(lowered.Module! with { CancellationIdentity = new(24, "1.23") }, "identity metadata on legacy artifact");
        var reader = module.Imports.Single(import => import.Id == GuestTaskCancellationIdentity.ReadImportId);
        var writer = module.Imports.Single(import => import.Id == GuestTaskCancellationIdentity.CancelImportId);
        foreach (var changed in new[] { reader with { Module = "env" }, reader with { ReturnTypeId = "type:int32" },
            reader with { ParameterTypeIds = Array.Empty<string>() }, reader with { DispatchClass = "binding" },
            reader with { Name = "avid_task_cancellation_token_v2" }, reader with { Id = "import:alias" },
            reader with { OptimizationClass = "pure" }, reader with { BindingOrdinal = 0 } })
            Reject(module with { Imports = module.Imports.Select(import => import == reader ? changed : import).ToArray() }, "noncanonical reader");
        Reject(module with { Imports = module.Imports.Where(import => import != reader).ToArray() }, "missing reader");
        Reject(module with { Imports = module.Imports.Append(reader with { Id = "import:alias" }).ToArray() }, "aliased duplicate reader");
        Reject(module with { Imports = module.Imports.Append(lowered.Module!.Imports.Single(import =>
            import.Id == GuestTaskCancellationErrorValidator.ImportId)).ToArray() }, "retained legacy writer");
        Reject(module with { Imports = module.Imports.Select(import => import == writer
            ? writer with { ParameterTypeIds = writer.ParameterTypeIds.Take(4).ToArray() } : import).ToArray() }, "legacy writer signature");
        foreach (int schema in new[] { 24, 25, 26, 29, 30, 31 })
            Reject(lowered.Module! with { SchemaVersion = schema, IrVersion = "1." + (schema - 1),
                Imports = lowered.Module.Imports.Append(reader).ToArray() }, "new reader on legacy IR " + schema);

        var guard = module.DirectAwaitReadiness!.Guards[0];
        GuestModule Rewrite(string functionId, string blockId, Func<GuestInstruction, GuestInstruction> update) => module with
        {
            Functions = module.Functions.Select(function => function.Id != functionId ? function : function with
            { Blocks = function.Blocks.Select(block => block.Id != blockId ? block : block with
                { Instructions = block.Instructions.Select(update).ToArray() }).ToArray() }).ToArray(),
        };
        Reject(Rewrite(guard.FunctionId, guard.CancellationBlockId + ":task_created", instruction => instruction.TargetId == writer.Id
            ? instruction with { OperandIds = instruction.OperandIds.Take(4).Append(instruction.OperandIds[0]).ToArray() } : instruction),
            "Task token substituted for the evaluated source token");
        var pending = module.DirectAwaitRoutes![0];
        string pendingFunction = "function:synthetic:async_resume:" + pending.CallbackId;
        string pendingBlock = pending.NormalTargetBlockId + ":entry:cancel_path:task_created";
        var call = module.Functions.Single(function => function.Id == pendingFunction).Blocks.Single(block => block.Id == pendingBlock)
            .Instructions.Single(instruction => instruction.TargetId == writer.Id);
        Reject(Rewrite(pendingFunction, pendingBlock, instruction => instruction.ResultId == call.OperandIds[4]
            ? instruction with { Constant = new("int64", "-1") } : instruction), "fabricated pending source");
        Reject(Rewrite(pendingFunction, pendingBlock, instruction => instruction.TargetId == writer.Id
            ? instruction with { OperandIds = instruction.OperandIds.Take(4).ToArray() } : instruction), "missing source proof");
        Reject(module with { DirectAwaitRoutes = module.DirectAwaitRoutes.Skip(1).ToArray() }, "unlisted pending cancellation", null);

        foreach (string member in new[] { "base_schema_version", "base_ir_version", "future_field" })
        {
            var document = JsonNode.Parse(GuestIrSerializer.Serialize(module))!.AsObject();
            var plan = document["cancellation_identity"]!.AsObject();
            if (member == "future_field") plan[member] = 1; else plan.Remove(member);
            bool rejected = false;
            try { GuestIrSerializer.Deserialize(JsonSerializer.SerializeToUtf8Bytes(document)); }
            catch (InvalidDataException) { rejected = true; }
            Check(rejected, "Serializer rejects missing or unknown identity member: " + member);
        }
        const string catchSource = """
            using System;
            using System.Threading.Tasks;
            using AvidScript;
            public static class Script {
                public static async Task<int> Run() {
                    try { await AvidContinuations.NextTickAsync(); return 1; }
                    catch (OperationCanceledException error) { return error == null ? 0 : 7; }
                }
            }
            """;
        const string catchSourceId = "Scripts/AsyncCatchVariables.cs";
        var catchSemantic = SemanticAnalyzer.Analyze(catchSource, catchSourceId,
            FrontendAnalyzer.Analyze(catchSource, catchSourceId).Source.Sha256,
            new[] { new SemanticReferenceSource(CSharpGuestContinuationTests.ReferenceFacade,
                "generated://Continuations.cs", true) }, new SemanticCompilerWorkspace(),
            enableAsyncExceptionFlow: true, enableDirectAwaitCleanup: true, enableAsyncCancellationFlow: true,
            enableAsyncSynchronousExceptions: true, enableAsyncCatchVariables: true);
        Check(SemanticContract.HasAsyncCatchVariables(catchSemantic)
            && SemanticAsyncInvocationValidator.IsValid(catchSemantic), "Catch variable source has a validated semantic contract");
        foreach (var artifact in new[] { catchSemantic, catchSemantic with { SemanticVersion = "1.59" },
            catchSemantic with { SchemaVersion = 50 } })
        {
            string catchHash = Convert.ToHexString(SHA256.HashData(SemanticSerializer.Serialize(artifact))).ToLowerInvariant();
            foreach (bool languageErrors in new[] { false, true })
            {
                var rejected = CSharpGuestLowerer.Lower(artifact, catchHash, enableAsyncLanguageErrors: languageErrors);
                Check(!rejected.Succeeded && rejected.Module is null && rejected.Diagnostics.Any(d => d.Code == "ASCG1027"),
                    "Catch variable publication requires owned exception values in both lowering modes");
            }
            Check(!CSharpLanguageErrorCompiler.TryLower(artifact, catchHash, out var rejectedError, out var catchError)
                && rejectedError is null && catchError?.Contains("owned exception-value", StringComparison.Ordinal) == true,
                "Direct language-error lowering cannot bypass the catch publication boundary");
        }
        return count;
    }
}
