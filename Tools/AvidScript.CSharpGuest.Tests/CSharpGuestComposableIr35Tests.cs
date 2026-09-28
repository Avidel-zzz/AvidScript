using System;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestComposableIr35Tests
{
    public static int Run()
    {
        int count = 0;
        void Check(bool valid, string reason)
        {
            if (!valid) throw new InvalidOperationException("Composable IR 35: " + reason);
            count++;
        }

        const string combinedSource = """
            using System.Runtime.InteropServices;
            using System.Threading;
            public static class Script {
                static int State = 1;
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static void Begin() {}
                [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
                public static void End() {}
                [UnmanagedCallersOnly(EntryPoint = "run")]
                public static int Run() {
                    State += 1;
                    return CancellationToken.None == CancellationToken.None ? State : 0;
                }
            }
            """;
        const string combinedId = "Scripts/ComposableStaticToken.cs";
        var combinedSemantic = SemanticAnalyzer.Analyze(combinedSource, combinedId,
            FrontendAnalyzer.Analyze(combinedSource, combinedId).Source.Sha256,
            Array.Empty<SemanticReferenceSource>(), new SemanticCompilerWorkspace(),
            enableAsyncExceptionFlow: true, enableDirectAwaitCleanup: true,
            enableAsyncCancellationFlow: true, enableStaticInitialization: true,
            enableAsyncSynchronousExceptions: true, enableAsyncCatchVariables: true,
            enableCancellationTokens: true);
        Check(SemanticComposableCapabilities.IsVersion(combinedSemantic)
            && SemanticStaticInitializationValidator.IsValid(combinedSemantic)
            && SemanticCancellationTokenValidator.IsValid(combinedSemantic),
            "one C# source has a validated static-plus-token Semantic 54 contract");
        byte[] combinedSemanticBytes = SemanticSerializer.Serialize(combinedSemantic);
        string combinedHash = Convert.ToHexString(SHA256.HashData(combinedSemanticBytes)).ToLowerInvariant();
        Check(CSharpStaticInitializationCompiler.TryLower(combinedSemantic, combinedHash,
            out var sameSource, out string? combinedError) && sameSource is not null,
            "same-source static/token lowering: " + combinedError);
        Check(sameSource!.SchemaVersion == GuestComposableCapabilities.SchemaVersion
            && sameSource.Provenance.SemanticSchemaVersion == SemanticComposableCapabilities.SchemaVersion
            && sameSource.CapabilityManifest?.Capabilities.Count == 2
            && GuestModuleValidator.Validate(sameSource).Succeeded,
            "same-source IR 35 preserves composition and source provenance");
        Check(sameSource.StaticStorage?.Slots.Any(slot => slot.Id.StartsWith("static:$initialization:", StringComparison.Ordinal)) == true
            && sameSource.Types.Any(type => type.Id == GuestCancellationTokens.TypeId)
            && sameSource.Functions.SelectMany(function => function.Blocks)
                .SelectMany(block => block.Instructions).Any(instruction => instruction.TargetId == GuestCancellationTokens.FieldId),
            "the lowered module carries executable static guards and token-value operations");
        byte[] sameSourceBytes = GuestIrSerializer.Serialize(sameSource);
        Check(GuestModuleValidator.Validate(GuestIrSerializer.Deserialize(sameSourceBytes)).Succeeded
            && sameSourceBytes.SequenceEqual(GuestIrSerializer.Serialize(GuestIrSerializer.Deserialize(sameSourceBytes)))
            && CSharpStaticInitializationCompiler.TryLower(SemanticSerializer.Deserialize(combinedSemanticBytes), combinedHash,
                out var repeated, out combinedError)
            && sameSourceBytes.SequenceEqual(GuestIrSerializer.Serialize(repeated!)),
            "same-source composition is canonical and deterministic: " + combinedError);
        Check(!CSharpStaticInitializationCompiler.TryLower(combinedSemantic with { CapabilityManifest =
                combinedSemantic.CapabilityManifest! with { Capabilities = Array.Empty<SemanticCapability>() } },
                combinedHash, out var rejected, out _) && rejected is null,
            "the compiler rejects a source manifest that omits either capability");
        WasmCompilationResult sameSourceWasm = WasmModuleCompiler.Compile(sameSource);
        Check(sameSourceWasm.Succeeded,
            "same-source IR 35 emits WASM: " + string.Join(" | ",
                sameSourceWasm.Diagnostics.Select(item => item.Code + ": " + item.Message)));
        string sameSourceProvenance = WasmArtifactInspector.Inspect(sameSourceWasm.Bytes)
            .CustomSections.Single(section => section.Name == "avidscript.provenance").PayloadText;
        Check(sameSourceProvenance.Contains("guest_ir=35/1.34\nexecution_base=17/1.16\n", StringComparison.Ordinal)
            && sameSourceProvenance.Contains(
                "capabilities=error.cancellation_token_value@1,managed.static_storage@1\nsource_language=csharp\nsemantic=54/1.63",
                StringComparison.Ordinal)
            && WasmArtifactInspector.Inspect(sameSourceWasm.Bytes).CustomSections.Any(
                section => section.Name == "avidscript.language_errors"),
            "same-source WASM binds the exact execution base, capabilities, source contract and error catalog");
        Check(sameSourceWasm.Bytes.SequenceEqual(WasmModuleCompiler.Compile(sameSource).Bytes),
            "same-source IR 35 emits deterministic WASM bytes");
        Check(sameSource.ModuleId == "csharp:" + combinedId,
            "same-source module keeps its canonical C# source identity");

        const string asyncSource = """
            using System; using System.Runtime.InteropServices; using System.Threading; using System.Threading.Tasks; using AvidScript;
            public static class Cache { public static int State = 1; }
            public static class Script {
                public static int Result;
                [AvidTransient] private static AvidCancellationSource Source;
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static async void BeginPlay() { Result = await Run(); }
                [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
                public static void EndPlay() { Source.Cancel(); Source.Release(); }
                [UnmanagedCallersOnly(EntryPoint = "avid_cancel")]
                public static void Cancel() { Source.Cancel(); }
                [UnmanagedCallersOnly(EntryPoint = "avid_get_result")]
                public static int GetResult() => Result;
                public static async Task<int> Run() {
                    Source = AvidCancellationSource.Create();
                    CancellationToken token = Source.Token;
                    try {
                        await AvidContinuations.NextTickAsync().WithCancellation(token);
                        return Cache.State;
                    }
                    catch (OperationCanceledException error) {
                        return error.CancellationToken == token ? 6 + Cache.State : 9;
                    }
                }
            }
            """;
        string asyncFacade = CSharpGuestContinuationTests.ReferenceFacade
            .Replace("internal AvidCancellationToken(long value) { Value = value; }",
                "internal AvidCancellationToken(long value) { Value = value; } [MethodImpl(MethodImplOptions.InternalCall)] public static extern implicit operator System.Threading.CancellationToken(AvidCancellationToken token);", StringComparison.Ordinal)
            .Replace("public AvidDelayAwaitable WithCancellation(AvidCancellationToken token) => default;",
                "public AvidDelayAwaitable WithCancellation(AvidCancellationToken token) => default; public AvidDelayAwaitable WithCancellation(System.Threading.CancellationToken token) => default;", StringComparison.Ordinal)
            + CSharpGuestAsyncThrowRoutingTests.CancelFacade;
        const string asyncId = "Scripts/ComposableAsyncStaticToken.cs";
        var asyncSemantic = SemanticAnalyzer.Analyze(asyncSource, asyncId,
            FrontendAnalyzer.Analyze(asyncSource, asyncId).Source.Sha256,
            new[] { new SemanticReferenceSource(asyncFacade, "generated://Continuation.cs", true) },
            new SemanticCompilerWorkspace(), enableAsyncExceptionFlow: true,
            enableDirectAwaitCleanup: true, enableAsyncCancellationFlow: true,
            enableStaticInitialization: true, enableAsyncSynchronousExceptions: true,
            enableAsyncCatchVariables: true, enableCancellationTokens: true);
        Check(SemanticComposableCapabilities.IsVersion(asyncSemantic)
            && asyncSemantic.CapabilityManifest?.Capabilities.Count == 5
            && SemanticStaticInitializationValidator.IsValid(asyncSemantic)
            && SemanticCancellationTokenValidator.IsValid(asyncSemantic),
            "one async C# source carries five validated Semantic capabilities");
        string asyncHash = Convert.ToHexString(SHA256.HashData(SemanticSerializer.Serialize(asyncSemantic))).ToLowerInvariant();
        Check(CSharpStaticInitializationCompiler.TryLower(asyncSemantic, asyncHash,
                out var asyncModule, out var asyncError) && asyncModule is not null
            && asyncModule.CapabilityManifest is { ExecutionBaseSchemaVersion: 29, ExecutionBaseIrVersion: "1.28" }
            && asyncModule.CapabilityManifest.Capabilities.Count == 5
            && GuestModuleValidator.Validate(asyncModule).Succeeded,
            "same-source async IR 35 must pass direct validation: " + asyncError);
        GuestModule acceptedAsync = asyncModule!;
        Check(acceptedAsync.Exports.Any(export => export.Name == "avid_get_result")
            && acceptedAsync.Exports.Any(export => export.Name == "avid_cancel")
            && acceptedAsync.Exports.Any(export => export.Name == "avid_on_end_play"),
            "same-source async fixture exposes result, cancellation and teardown for native execution checks");
        byte[] asyncBytes = GuestIrSerializer.Serialize(acceptedAsync);
        Check(GuestModuleValidator.Validate(GuestIrSerializer.Deserialize(asyncBytes)).Succeeded
            && asyncBytes.SequenceEqual(GuestIrSerializer.Serialize(GuestIrSerializer.Deserialize(asyncBytes))),
            "same-source async IR 35 keeps a canonical validated round trip");
        var missingExceptionCapability = GuestModuleValidator.Validate(acceptedAsync with { CapabilityManifest =
            acceptedAsync.CapabilityManifest! with { Capabilities = acceptedAsync.CapabilityManifest.Capabilities
                .Where(capability => capability.Id != GuestComposableCapabilities.ExceptionValues).ToArray() } });
        Check(!missingExceptionCapability.Succeeded
            && missingExceptionCapability.Diagnostics.Any(item => item.Code == "ASIR1042"),
            "a published async module cannot omit its exception-value capability");
        Check(GuestModuleValidator.Validate(acceptedAsync with { AsyncExceptionTransfers = null })
            .Diagnostics.Any(item => item.Code == "ASIR1042"),
            "IR 35 async base requires exception transfer metadata");
        Check(CSharpStaticInitializationCompiler.TryLower(SemanticSerializer.Deserialize(
                SemanticSerializer.Serialize(asyncSemantic)), asyncHash, out var repeatedAsync, out asyncError)
            && asyncBytes.SequenceEqual(GuestIrSerializer.Serialize(repeatedAsync!)),
            "same-source async IR 35 lowering is deterministic: " + asyncError);
        WasmCompilationResult asyncWasm = WasmModuleCompiler.Compile(acceptedAsync);
        Check(asyncWasm.Succeeded,
            "same-source async IR 35 emits WASM: " + string.Join(" | ",
                asyncWasm.Diagnostics.Select(item => item.Code + ": " + item.Message)));
        string asyncProvenance = WasmArtifactInspector.Inspect(asyncWasm.Bytes)
            .CustomSections.Single(section => section.Name == "avidscript.provenance").PayloadText;
        Check(asyncProvenance.Contains("guest_ir=35/1.34\ntask_local_exception_model=cancellation\nexecution_base=29/1.28\n",
                StringComparison.Ordinal)
            && asyncProvenance.Contains("capabilities=async.await_readiness@1,async.cancellation_identity@1,error.cancellation_token_value@1,error.exception_values@1,managed.static_storage@1\nsource_language=csharp\nsemantic=54/1.63",
                StringComparison.Ordinal)
            && asyncWasm.Bytes.SequenceEqual(WasmModuleCompiler.Compile(acceptedAsync).Bytes),
            "same-source async WASM preserves its five capabilities and deterministic bytes");
        Check(!CSharpStaticInitializationCompiler.TryLower(asyncSemantic with { CapabilityManifest =
                asyncSemantic.CapabilityManifest! with { Capabilities = Array.Empty<SemanticCapability>() } },
                asyncHash, out var omittedModule, out _)
            && omittedModule is null,
            "private async execution cannot skip a missing five-capability source declaration");
        string? fixtureDirectory = Environment.GetEnvironmentVariable("AVIDSCRIPT_COMPOSABLE_IR35_FIXTURE_DIR");
        if (!string.IsNullOrWhiteSpace(fixtureDirectory))
        {
            Directory.CreateDirectory(fixtureDirectory);
            File.WriteAllText(Path.Combine(fixtureDirectory, "composable-static-token.cs"), combinedSource);
            File.WriteAllBytes(Path.Combine(fixtureDirectory, "composable-static-token.semantic.json"), combinedSemanticBytes);
            File.WriteAllBytes(Path.Combine(fixtureDirectory, "composable-static-token.guestir.json"), sameSourceBytes);
            File.WriteAllBytes(Path.Combine(fixtureDirectory, "composable-static-token.wasm"), sameSourceWasm.Bytes);
            File.WriteAllText(Path.Combine(fixtureDirectory, "composable-async-static-token.cs"), asyncSource);
            File.WriteAllBytes(Path.Combine(fixtureDirectory, "composable-async-static-token.semantic.json"),
                SemanticSerializer.Serialize(asyncSemantic));
            File.WriteAllBytes(Path.Combine(fixtureDirectory, "composable-async-static-token.guestir.json"), asyncBytes);
            File.WriteAllBytes(Path.Combine(fixtureDirectory, "composable-async-static-token.wasm"), asyncWasm.Bytes);
        }

        const string source = """
            using System.Runtime.InteropServices;
            public class Cache<T> { public static int Value; }
            public static class Script {
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")] public static void Begin() {}
                [UnmanagedCallersOnly(EntryPoint = "run")] public static int ExportRun() { return Run(); }
                public static int Run() {
                    Cache<int>.Value += 1; Cache<long>.Value += 2;
                    return Cache<int>.Value * 10 + Cache<long>.Value;
                }
            }
            """;
        const string sourceId = "Scripts/ComposableStaticBase17.cs";
        var semantic = SemanticAnalyzer.Analyze(source, sourceId,
            FrontendAnalyzer.Analyze(source, sourceId).Source.Sha256,
            Array.Empty<SemanticReferenceSource>(), new SemanticCompilerWorkspace(), enableStaticInitialization: true);
        byte[] semanticBytes = SemanticSerializer.Serialize(semantic);
        string semanticHash = Convert.ToHexString(SHA256.HashData(semanticBytes)).ToLowerInvariant();
        Check(CSharpStaticInitializationCompiler.TryLower(semantic, semanticHash, out var staticModule, out string? error)
            && staticModule is not null, "C# static initializer lowering: " + error);
        Check(staticModule!.SchemaVersion == 27 && staticModule.StaticStorage is { BaseSchemaVersion: 17, BaseIrVersion: "1.16" }
            && staticModule.LanguageOutcomeTypes is not null && staticModule.LanguageErrorCatalog is not null,
            "C# static initialization needs the language-error base even with an int field");

        GuestType[] types = staticModule.Types.Any(type => type.Id == "type:int64")
            ? staticModule.Types.Append(GuestCancellationTokens.ValueType()).ToArray()
            : staticModule.Types.Concat(new[] {
                new GuestType("type:int64", "scalar", "i64", Array.Empty<GuestField>(), null, null, 8, 8),
                GuestCancellationTokens.ValueType(),
            }).ToArray();
        GuestLayoutResult layout = GuestLayoutBuilder.Build(types, staticModule.Globals, staticModule.DataSegments);
        Check(layout.Succeeded && layout.Layout is not null, "combined module layout");
        GuestFunction tokenProbe = new("function:token_probe", Array.Empty<GuestRegister>(), new[] {
            new GuestRegister("identity", "type:int64"), new GuestRegister("token", GuestCancellationTokens.TypeId),
            new GuestRegister("roundtrip", "type:int64"),
        }, "type:int64", "entry", new[] { new GuestBasicBlock("entry", new[] {
            new GuestInstruction("constant", "identity", Array.Empty<string>(), null, null, new("int64", "7")),
            new GuestInstruction("stack_alloc", "token", Array.Empty<string>(), null, null, null),
            new GuestInstruction("field_store", null, new[] { "token", "identity" }, GuestCancellationTokens.FieldId, null, null),
            new GuestInstruction("field_load", "roundtrip", new[] { "token" }, GuestCancellationTokens.FieldId, null, null),
        }, new GuestTerminator("return", null, null, null, "roundtrip")) });
        GuestModule composed = staticModule with
        {
            SchemaVersion = GuestComposableCapabilities.SchemaVersion,
            IrVersion = GuestComposableCapabilities.IrVersion,
            // The token probe is hand-added to an existing C# IR 27 module.
            // Keep its old source provenance visible; this is not C# Semantic 54 lowering.
            Language = "guest-ir",
            Types = types,
            MemoryLayout = layout.Layout!,
            Functions = staticModule.Functions.Append(tokenProbe).ToArray(),
            CancellationTokens = new(17, "1.16"),
            CapabilityManifest = GuestCapabilityManifest.Create(17, "1.16", new[] {
                new GuestCapability(GuestComposableCapabilities.StaticStorage, 1),
                new GuestCapability(GuestComposableCapabilities.CancellationTokenValue, 1),
            }),
        };
        GuestValidationResult validation = GuestModuleValidator.Validate(composed);
        Check(validation.Succeeded, "real static-initializer module plus token value: "
            + string.Join(" | ", validation.Diagnostics.Select(item => item.Code + ": " + item.Message)));
        byte[] bytes = GuestIrSerializer.Serialize(composed);
        Check(GuestModuleValidator.Validate(GuestIrSerializer.Deserialize(bytes)).Succeeded
            && bytes.SequenceEqual(GuestIrSerializer.Serialize(GuestIrSerializer.Deserialize(bytes))),
            "canonical base-17 static initialization and token IR");
        Check(!GuestModuleValidator.Validate(composed with { Language = "csharp" }).Succeeded,
            "a composed C# artifact cannot claim an older Semantic source version");
        Check(!GuestModuleValidator.Validate(composed with { LanguageErrorCatalog = null }).Succeeded,
            "base 17 cannot omit its language-error catalog");
        Check(!GuestModuleValidator.Validate(composed with { LanguageOutcomeTypes = null }).Succeeded,
            "base 17 cannot omit its outcome layout");
        Check(!GuestModuleValidator.Validate(composed with { CapabilityManifest = composed.CapabilityManifest! with
            { ExecutionBaseSchemaVersion = 14, ExecutionBaseIrVersion = "1.13" } }).Succeeded,
            "base 17 plans cannot be relabeled as base 14");
        Check(!GuestModuleValidator.Validate(composed with { Types = composed.Types.Where(type =>
            type.Id != GuestCancellationTokens.TypeId).ToArray() }).Succeeded,
            "base 17 token plan requires the nominal value type");
        var badReport = composed with { Imports = composed.Imports.Select(import =>
            import.Id == "import:language_error_report_v1"
                ? import with { ParameterTypeIds = new[] { "type:int32", "type:int32", "type:int64" } }
                : import).ToArray() };
        Check(GuestModuleValidator.Validate(badReport).Diagnostics.Any(item => item.Code == "ASIR1013"),
            "base 17 cannot weaken the managed error-report import signature: "
                + string.Join(", ", composed.Imports.Select(import => import.Id)));
        Check(GuestModuleValidator.Validate(composed with { LanguageErrorCatalog = composed.LanguageErrorCatalog! with
            { Sources = composed.LanguageErrorCatalog.Sources.Select((site, index) =>
                index == 0 ? site with { Start = -1 } : site).ToArray() } }).Diagnostics.Any(item => item.Code == "ASIR1027"),
            "base 17 cannot bypass language-error source bounds");
        WasmCompilationResult composedWasm = WasmModuleCompiler.Compile(composed);
        Check(composedWasm.Succeeded,
            "validated guest-ir IR 35 emits WASM: " + string.Join(" | ",
                composedWasm.Diagnostics.Select(item => item.Code + ": " + item.Message)));
        Check(WasmArtifactInspector.Inspect(composedWasm.Bytes).CustomSections.Single(
                section => section.Name == "avidscript.provenance").PayloadText.Contains(
                "source_language=guest-ir\nsemantic=", StringComparison.Ordinal),
            "hand-composed guest IR retains its own source-language identity");
        return count;
    }
}
