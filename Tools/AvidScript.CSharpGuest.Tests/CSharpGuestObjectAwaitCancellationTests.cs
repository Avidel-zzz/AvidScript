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

internal static class CSharpGuestObjectAwaitCancellationTests
{
    internal static int Run()
    {
        int count = 0;
        void Check(bool valid, string reason) { if (!valid) throw new InvalidOperationException(reason); count++; }
        foreach (bool staticState in new[] { false, true })
        foreach (bool tokens in new[] { false, true })
        foreach (bool voidOwner in new[] { false, true })
        foreach (bool namedCatch in new[] { false, true })
        {
            string name = $"object-static{staticState}-token{tokens}-void{voidOwner}-catch{namedCatch}";
            string source = "using System; using System.Threading; using System.Threading.Tasks; using System.Runtime.InteropServices; using AvidScript; "
                + "public static class Script { " + (staticState ? "static int State = 1; " : "")
                + "[UnmanagedCallersOnly(EntryPoint = \"object_main\")] public static int Main() { return 0; } "
                + "public static async " + (voidOwner ? "void" : "Task<int>") + " Run("
                + (tokens ? "CancellationToken token" : "") + ") { try { "
                + "var loaded = await AvidAssets.LoadObjectAsync(\"/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture\")"
                + (tokens ? ".WithCancellation(token)" : "") + "; "
                + (voidOwner ? "int slot = loaded.Slot; return;" : "return loaded.Slot;")
                + " } " + (namedCatch ? "catch (OperationCanceledException error) { "
                    + (voidOwner ? "return;" : "return 7;") + " } " : "")
                + "finally { " + (staticState ? "State = State + 1;" : "int cleanup = 1;") + " } } }";
            var semantic = Analyze(source, voidOwner);
            byte[] input = SemanticSerializer.Serialize(semantic);
            string hash = Convert.ToHexString(SHA256.HashData(input)).ToLowerInvariant();
            Check(SemanticObjectAwaitCancellationValidator.IsValid(semantic) && SemanticObjectAwaitCancellation.Has(semantic), name + " source");
            Check(CSharpLanguageCapabilityCompiler.TryLower(semantic, hash, out var module, out var error)
                && module is not null, name + " lowering: " + error);
            Check(GuestObjectAwaitCancellation.HasDeclaredExecutionBase(module!) && GuestModuleValidator.Validate(module!).Succeeded,
                name + " independent IR admission");
            var wasm = WasmModuleCompiler.Compile(module!);
            Check(wasm.Succeeded, name + " WASM: " + string.Join(" | ", wasm.Diagnostics.Select(item => item.Message)));
            var json = GuestIrSerializer.Serialize(module!);
            Check(json.SequenceEqual(GuestIrSerializer.Serialize(GuestIrSerializer.Deserialize(json)))
                && wasm.Bytes.SequenceEqual(WasmModuleCompiler.Compile(GuestIrSerializer.Deserialize(json)).Bytes), name + " canonical round trip");
            Check(CSharpLanguageCapabilityCompiler.TryLower(semantic, hash, out var repeated, out _)
                && json.SequenceEqual(GuestIrSerializer.Serialize(repeated!)), name + " deterministic lowering");
            Check(input.SequenceEqual(SemanticSerializer.Serialize(semantic)), name + " caller source unchanged");
            Check(!CSharpGuestLowerer.Lower(semantic, hash).Succeeded, name + " no ordinary-lowerer bypass");
            Check(!GuestModuleValidator.Validate(module! with { ObjectAwaitCancellation = null }).Succeeded, name + " missing plan rejected");
            Check(!GuestModuleValidator.Validate(module! with { SchemaVersion = 38, IrVersion = "1.37" }).Succeeded, name + " old envelope rejected");
            Check(!GuestModuleValidator.Validate(module! with { SchemaVersion = 40, IrVersion = "1.39" }).Succeeded, name + " future envelope rejected");
            string[] provenance = WasmArtifactInspector.Inspect(wasm.Bytes).CustomSections.Single(section =>
                section.Name == "avidscript.provenance").PayloadText.Split('\n');
            Check(provenance.Contains("guest_ir=39/1.38") && provenance.Contains("semantic=58/1.67")
                && provenance.Contains("execution_base=29/1.28")
                && provenance.Contains($"source_execution={module!.ObjectAwaitCancellation!.SourceBaseSchemaVersion}/{module.ObjectAwaitCancellation.SourceBaseSemanticVersion}"), name + " canonical source/execution provenance");
            using var admission = JsonDocument.Parse(CSharpLanguageProfileAdmission.Describe("gameplay-v1", input));
            Check(admission.RootElement.GetProperty("guest_schema_version").GetInt32() == 39
                && admission.RootElement.GetProperty("guest_ir_sha256").GetString() == Convert.ToHexString(SHA256.HashData(json)).ToLowerInvariant(), name + " public build admission uses exact validated IR bytes");
            if (!staticState && !tokens && !voidOwner && !namedCatch) CheckTampering(module!, Check);
        }
        foreach (var (name, body) in new[] {
            ("implicit", "var loaded = await AvidAssets.LoadObjectAsync(\"/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture\"); return loaded.Slot;"),
            ("captured", "try { var loaded = await AvidAssets.LoadObjectAsync(\"/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture\"); Func<int> read = () => loaded.Slot; await AvidContinuations.NextTickAsync(); return read(); } finally { int cleanup = 1; }"),
            ("nested", "try { try { var first = await AvidAssets.LoadObjectAsync(\"/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture\"); var second = await AvidAssets.LoadObjectAsync(\"/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture\"); return first.Slot + second.Slot; } finally { State = State + 1; } } finally { State = State + 10; }") })
        {
            string source = "using System; using System.Threading.Tasks; using System.Runtime.InteropServices; using AvidScript; public static class Script { " + (name == "captured" ? "" : "static int State; ") + "[UnmanagedCallersOnly(EntryPoint = \"object_main\")] public static int Main() => 0; public static async Task<int> Run() { " + body + " } }";
            var semantic = Analyze(source);
            string hash = Convert.ToHexString(SHA256.HashData(SemanticSerializer.Serialize(semantic))).ToLowerInvariant();
            Check(CSharpLanguageCapabilityCompiler.TryLower(semantic, hash, out var module, out var error), name + ": " + error
                + $" source={semantic.SchemaVersion}/{semantic.SemanticVersion}; " + string.Join(" | ", semantic.Diagnostics.Select(item => item.Code + ": " + item.Message)));
            Check(GuestModuleValidator.Validate(module!).Succeeded && WasmModuleCompiler.Compile(module!).Succeeded, name + " complete payload/cleanup composition");
        }
        const string staticDelegate = "using System; using System.Threading.Tasks; using System.Runtime.InteropServices; using AvidScript; public static class Script { static int State; [UnmanagedCallersOnly(EntryPoint = \"object_main\")] public static int Main() => 0; public static async Task<int> Run() { try { var loaded = await AvidAssets.LoadObjectAsync(\"/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture\"); Func<int> read = () => loaded.Slot; await AvidContinuations.NextTickAsync(); return read(); } finally { State = State + 1; } } }";
        var unsupported = Analyze(staticDelegate);
        string unsupportedHash = Convert.ToHexString(SHA256.HashData(SemanticSerializer.Serialize(unsupported))).ToLowerInvariant();
        Check(!CSharpLanguageCapabilityCompiler.TryLower(unsupported, unsupportedHash, out var rejectedModule, out var rejection)
            && rejectedModule is null && rejection?.Contains("outcome effect closure", StringComparison.Ordinal) == true,
            "static captured delegate needs its outcome adapter and stays fail-closed");
        return count;
    }

    private static void CheckTampering(GuestModule module, Action<bool, string> check)
    {
        void Reject(GuestModule input, string name) => check(!GuestModuleValidator.Validate(input).Succeeded
            && !WasmModuleCompiler.Compile(input).Succeeded, "object IR rejects " + name);
        var plan = module.ObjectAwaitCancellation!;
        Reject(module with { ObjectAwaitCancellation = plan with { AwaitCallbackIds = Array.Empty<int>() } }, "empty callback plan");
        Reject(module with { ObjectAwaitCancellation = plan with { AwaitCallbackIds = new[] { plan.AwaitCallbackIds[0] + 1 } } }, "unlisted callback");
        Reject(module with { ObjectAwaitCancellation = plan with { AwaitCallbackIds = plan.AwaitCallbackIds.Concat(plan.AwaitCallbackIds).ToArray() } }, "duplicate callback");
        Reject(module with { ObjectAwaitCancellation = plan with { SourceBaseSemanticVersion = "1.58" } }, "wrong source base pair");
        Reject(module with { ObjectAwaitCancellation = plan with { SourceBaseSchemaVersion = 53, SourceBaseSemanticVersion = "1.62" } }, "invented token source base");
        Reject(module with { CapabilityManifest = module.CapabilityManifest! with { Capabilities = module.CapabilityManifest.Capabilities.Where(capability => capability.Id != GuestObjectAwaitCancellation.CapabilityId).ToArray() } }, "missing capability");
        foreach (var (name, mutate) in new (string, Func<GuestImport, GuestImport>)[] {
            ("wrong import module", import => import with { Module = "env" }),
            ("future import", import => import with { Name = "avid_continuation_load_object_cancel_resume_v2" }),
            ("legacy downgrade", import => import with { Module = "env", Name = "continuation_load_object" }),
            ("narrow token", import => import with { ReturnTypeId = "type:int32" }),
            ("wrong arguments", import => import with { ParameterTypeIds = new[] { "type:int32", "type:int32" } }) })
            Reject(module with { Imports = module.Imports.Select(import => import.Name == GuestObjectAwaitCancellation.ImportName ? mutate(import) : import).ToArray() }, name);
        GuestModule MutateBlocks(Func<GuestFunction, GuestBasicBlock, GuestBasicBlock> mutate) => module with {
            Functions = module.Functions.Select(function => function with { Blocks = function.Blocks.Select(block => mutate(function, block)).ToArray() }).ToArray() };
        Reject(MutateBlocks((function, block) => block.Id.EndsWith(":failed_check", StringComparison.Ordinal)
            ? block with { Terminator = block.Terminator with { TargetBlockId = block.Terminator.FalseTargetBlockId } } : block), "failed object resumes cancellation");
        string resumeId = "function:synthetic:async_resume:" + plan.AwaitCallbackIds[0];
        string payloadId = module.Functions.Single(function => function.Id == resumeId).Parameters[2].Id;
        var loaded = module.Types.Single(type => type.Id == GuestObjectAwaitCancellation.LoadedObjectTypeId);
        Reject(MutateBlocks((function, block) => function.Id == resumeId && block.Id == function.EntryBlockId
            ? block with { Instructions = block.Instructions.Append(new GuestInstruction("field_load", function.Locals.First(local => local.TypeId == "type:int32").Id, new[] { payloadId }, loaded.Fields[0].Id, null, null)).ToArray() } : block), "object read before status admission");
        Reject(MutateBlocks((function, block) => function.Id == resumeId && block.Id.EndsWith(":entry:cancel_path", StringComparison.Ordinal)
            ? block with { Instructions = block.Instructions.Append(new GuestInstruction("field_load", function.Locals.First(local => local.TypeId == "type:int32").Id, new[] { payloadId }, loaded.Fields[0].Id, null, null)).ToArray() } : block), "object read during cancellation");
        string routerId = module.Exports.Single(export => export.Name == "avid_on_continuation_v2").FunctionId;
        Reject(MutateBlocks((function, block) => function.Id == routerId && block.Id.EndsWith(":check_failed", StringComparison.Ordinal)
            ? block with { Instructions = block.Instructions.Select(instruction => instruction.Constant is { Value: "2" } ? instruction with { Constant = instruction.Constant with { Value = "4" } } : instruction).ToArray() } : block), "invalid router status accepted");
        Reject(MutateBlocks((function, block) => function.Id == routerId && block.Instructions.Any(instruction => instruction.TargetId == resumeId)
            ? block with { Instructions = block.Instructions.Select(instruction => instruction.TargetId == loaded.Fields[0].Id ? instruction with { TargetId = loaded.Fields[1].Id } : instruction).ToArray() } : block), "swapped payload fields");
        foreach (string property in new[] { "source_base_schema_version", "source_base_semantic_version", "await_callback_ids" })
        {
            var json = JsonNode.Parse(GuestIrSerializer.Serialize(module))!.AsObject();
            json["object_await_cancellation"]!.AsObject().Remove(property);
            bool rejected = false;
            try { GuestIrSerializer.Deserialize(System.Text.Encoding.UTF8.GetBytes(json.ToJsonString())); } catch (InvalidDataException) { rejected = true; }
            check(rejected, "required object plan property rejected: " + property);
        }
        var unknown = JsonNode.Parse(GuestIrSerializer.Serialize(module))!.AsObject();
        unknown["object_await_cancellation"]!["unknown"] = 1;
        bool unknownRejected = false;
        try { GuestIrSerializer.Deserialize(System.Text.Encoding.UTF8.GetBytes(unknown.ToJsonString())); } catch (InvalidDataException) { unknownRejected = true; }
        check(unknownRejected, "unknown object plan property rejected");
    }

    internal static SemanticDocument Analyze(string source, bool voidOwner = false) =>
        SemanticAnalyzer.Analyze(source, "Scripts/ObjectAwaitCancellation.cs",
            FrontendAnalyzer.Analyze(source, "Scripts/ObjectAwaitCancellation.cs").Source.Sha256,
            new[] { new SemanticReferenceSource(Facade, "generated://ObjectAwaitCancellation.cs", true) },
            new SemanticCompilerWorkspace(), enableAsyncExceptionFlow: true, enableDirectAwaitCleanup: true,
            enableAsyncCancellationFlow: true, enableStaticInitialization: true, enableAsyncSynchronousExceptions: true,
            enableAsyncCatchVariables: true, enableCancellationTokens: true, enableAsyncVoidErrorOwner: voidOwner);

    internal static string Facade => CSharpGuestCancellationTokenTests.AsyncFacade.Replace(
        "internal static extern long ContinuationLoadObject(string assetPath, int callbackId);",
        "internal static extern long ContinuationLoadObject(string assetPath, int callbackId); "
            + "[DllImport(\"avidscript\", EntryPoint = \"avid_continuation_load_object_cancel_resume_v1\")] "
            + "internal static extern long ContinuationLoadObjectCancelResume(string assetPath, int callbackId);",
        StringComparison.Ordinal).Replace(
        "public AvidObjectAwaitable WithCancellation(AvidCancellationToken token) => default;",
        "public AvidObjectAwaitable WithCancellation(AvidCancellationToken token) => default; public AvidObjectAwaitable WithCancellation(System.Threading.CancellationToken token) => default;",
        StringComparison.Ordinal);
}
