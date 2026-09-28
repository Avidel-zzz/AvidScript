using System;
using System.IO;
using System.Linq;
using System.Text;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpSemantic;

internal static class SemanticComposableCapabilityTests
{
    public static int Run()
    {
        int count = 0;
        void Check(bool condition, string reason)
        {
            if (!condition) throw new InvalidOperationException("Composable Semantic: " + reason);
            count++;
        }

        SemanticDocument Analyze(string source, bool staticInitialization = true) =>
            SemanticAnalyzer.Analyze(source, "Scripts/Composable.cs",
                FrontendAnalyzer.Analyze(source, "Scripts/Composable.cs").Source.Sha256,
                new[] { new SemanticReferenceSource(Facade, "generated://Continuation.cs", true) },
                new SemanticCompilerWorkspace(), enableAsyncExceptionFlow: true,
                enableDirectAwaitCleanup: true, enableAsyncCancellationFlow: true,
                enableStaticInitialization: staticInitialization,
                enableAsyncSynchronousExceptions: true, enableAsyncCatchVariables: true,
                enableCancellationTokens: true);

        var document = Analyze(Source);
        Check(SemanticComposableCapabilities.IsVersion(document)
            && document.StaticInitialization is not null, "static and token source receives Semantic 54");
        Check(document.Diagnostics.All(d => d.Severity != "error" || d.Code is "ASCS3001" or "ASCS5422"),
            "source diagnostics: " + string.Join(" | ", document.Diagnostics.Select(d => d.Code + ": " + d.Message)));
        Check(SemanticComposableCapabilityValidator.IsValid(document)
            && SemanticStaticInitializationValidator.IsValid(document)
            && SemanticCancellationTokenValidator.IsValid(document)
            && SemanticAsyncInvocationValidator.IsValid(document), "all source plans validate together");
        string[] ids = new[] {
            SemanticComposableCapabilities.AwaitReadiness,
            SemanticComposableCapabilities.CancellationIdentity,
            SemanticComposableCapabilities.CancellationTokenValue,
            SemanticComposableCapabilities.ExceptionValues,
            SemanticComposableCapabilities.StaticStorage,
        }.OrderBy(id => id, StringComparer.Ordinal).ToArray();
        Check(document.CapabilityManifest!.Capabilities.Select(capability => capability.Id).SequenceEqual(ids),
            "capabilities are ordered by ordinal ID");
        Check(document.CapabilityManifest.BaseSchemaVersion == SemanticComposableCapabilities.AsyncBaseSchemaVersion
            && document.StaticInitialization!.BaseSchemaVersion == SemanticComposableCapabilities.AsyncBaseSchemaVersion,
            "source and static plan share the execution base");
        byte[] bytes = SemanticSerializer.Serialize(document);
        Check(bytes.SequenceEqual(SemanticSerializer.Serialize(Analyze(Source)))
            && bytes.SequenceEqual(SemanticSerializer.Serialize(SemanticSerializer.Deserialize(bytes))),
            "same source yields deterministic canonical bytes");
        Check(!SemanticComposableCapabilityValidator.IsValid(document with { SchemaVersion = 53, SemanticVersion = "1.62" })
            && !SemanticComposableCapabilityValidator.IsValid(document with { CapabilityManifest = null }),
            "old and missing envelopes reject combined plans");
        Check(!SemanticComposableCapabilityValidator.IsValid(document with { SchemaVersion = 53, CapabilityManifest = null })
            && !SemanticComposableCapabilityValidator.IsValid(document with { SemanticVersion = "1.62", CapabilityManifest = null }),
            "half-matched future versions cannot masquerade as old documents");
        Check(!SemanticComposableCapabilityValidator.IsValid(document with { CapabilityManifest =
                document.CapabilityManifest with { Capabilities = document.CapabilityManifest.Capabilities.Reverse().ToArray() } })
            && !SemanticComposableCapabilityValidator.IsValid(document with { CapabilityManifest =
                document.CapabilityManifest with { Capabilities = document.CapabilityManifest.Capabilities.Skip(1).ToArray() } }),
            "order and required plans are fixed");
        Check(!SemanticComposableCapabilityValidator.IsValid(document with { CapabilityManifest =
                document.CapabilityManifest with { Capabilities = document.CapabilityManifest.Capabilities.Select(
                    (capability, index) => index == 0 ? capability with { Id = "future.feature" } : capability).ToArray() } })
            && !SemanticComposableCapabilityValidator.IsValid(document with { CapabilityManifest =
                document.CapabilityManifest with { Capabilities = document.CapabilityManifest.Capabilities.Select(
                    (capability, index) => index == 0 ? capability with { Version = 2 } : capability).ToArray() } }),
            "unknown capability and future version reject");
        Check(!SemanticComposableCapabilityValidator.IsValid(document with { StaticInitialization =
                document.StaticInitialization! with { BaseSchemaVersion = 53 } })
            && !SemanticComposableCapabilityValidator.IsValid(document with { Types =
                document.Types.Where(type => type.Id != SemanticCancellationTokens.TypeId).ToArray() }),
            "conflicting base and missing token type reject");
        Check(!SemanticComposableCapabilityValidator.IsValid(document with { Types =
                document.Types.Append(null!).ToArray() }), "malformed type graph rejects without dereferencing null");
        string json = Encoding.UTF8.GetString(bytes);
        bool RejectJson(string artifact)
        {
            try { SemanticSerializer.Deserialize(Encoding.UTF8.GetBytes(artifact)); return false; }
            catch (InvalidDataException) { return true; }
        }
        Check(RejectJson(json.Replace("\"schema_version\": 54,", "\"schema_version\": 54,\n  \"schema_version\": 54,", StringComparison.Ordinal))
            && RejectJson(json.Replace("\"capability_manifest\":", "\"unknown_field\": true,\n  \"capability_manifest\":", StringComparison.Ordinal)),
            "new schema rejects duplicate and unknown JSON members");
        Check(RejectJson(json.Replace("\"schema_version\": 54,", "\"schema_version\": 53,", StringComparison.Ordinal)),
            "old serialized schema cannot smuggle the manifest");
        var old = Analyze(Source, staticInitialization: false);
        Check(old.CapabilityManifest is null && old.SchemaVersion == SemanticContract.CancellationTokenSchemaVersion
            && !json.Equals(Encoding.UTF8.GetString(SemanticSerializer.Serialize(old)), StringComparison.Ordinal),
            "existing token-only output remains on Semantic 53 without a manifest");
        var staticOnly = Analyze("public static class Script { public static int State = 1; }");
        Check(staticOnly.CapabilityManifest is null
            && staticOnly.SchemaVersion == SemanticStaticInitialization.SchemaVersion
            && SemanticStaticInitializationValidator.IsValid(staticOnly),
            "enabling token analysis does not invent a token capability when source has none: schema="
                + staticOnly.SchemaVersion + " diagnostics=" + string.Join(" | ", staticOnly.Diagnostics.Select(d => d.Message)));
        var tokenOnly = Analyze("using System.Threading; public static class Script { public static bool Run() => CancellationToken.None == CancellationToken.None; }");
        Check(tokenOnly.CapabilityManifest is null
            && tokenOnly.SchemaVersion == SemanticContract.CancellationTokenSchemaVersion
            && SemanticCancellationTokenValidator.IsValid(tokenOnly),
            "enabling static analysis does not invent static storage when source has none");
        const string synchronousSource = "using System.Threading; public static class Script { static int State = 1; public static bool Run() => CancellationToken.None == CancellationToken.None && State == 1; }";
        var synchronous = Analyze(synchronousSource);
        Check(SemanticComposableCapabilities.IsVersion(synchronous)
            && synchronous.CapabilityManifest?.Capabilities.Select(capability => capability.Id)
                .SequenceEqual(new[] { SemanticComposableCapabilities.CancellationTokenValue,
                    SemanticComposableCapabilities.StaticStorage }.OrderBy(id => id, StringComparer.Ordinal)) == true,
            "synchronous static and token source carries only its two projected capabilities");
        Check(synchronous.CapabilityManifest!.BaseSchemaVersion == SemanticComposableCapabilities.SynchronousBaseSchemaVersion
            && synchronous.StaticInitialization!.BaseSchemaVersion == SemanticComposableCapabilities.SynchronousBaseSchemaVersion
            && !SemanticContract.HasAsyncCatchVariables(synchronous)
            && !SemanticContract.HasAsyncSynchronousExceptions(synchronous),
            "synchronous token values do not inherit an async base or async exception semantics");
        Check(SemanticComposableCapabilityValidator.IsValid(synchronous)
            && SemanticStaticInitializationValidator.IsValid(synchronous)
            && SemanticCancellationTokenValidator.IsValid(synchronous)
            && SemanticAsyncInvocationValidator.IsValid(synchronous),
            "synchronous composition validates across the source contracts");
        Check(!SemanticComposableCapabilityValidator.IsValid(synchronous with { CapabilityManifest =
                synchronous.CapabilityManifest with { BaseSchemaVersion = SemanticComposableCapabilities.AsyncBaseSchemaVersion } })
            && !SemanticComposableCapabilityValidator.IsValid(synchronous with { CapabilityManifest =
                synchronous.CapabilityManifest with { Capabilities = document.CapabilityManifest.Capabilities } }),
            "synchronous source cannot claim an async base or plans");
        byte[] synchronousBytes = SemanticSerializer.Serialize(synchronous);
        Check(synchronousBytes.SequenceEqual(SemanticSerializer.Serialize(Analyze(synchronousSource)))
            && synchronousBytes.SequenceEqual(SemanticSerializer.Serialize(SemanticSerializer.Deserialize(synchronousBytes))),
            "synchronous combination has deterministic canonical bytes");
        return count;
    }

    private const string Source = """
        using System; using System.Threading; using System.Threading.Tasks; using AvidScript;
        public static class Script {
            private static int State = 1;
            public static async Task<int> Run() {
                CancellationToken token = CancellationToken.None;
                try {
                    await AvidContinuations.NextTickAsync().WithCancellation(token);
                    return State;
                }
                catch (OperationCanceledException error) {
                    return error.CancellationToken == token ? 7 : 9;
                }
            }
        }
        """;

    private const string Facade = """
        using System; using System.Runtime.CompilerServices; using System.Threading;
        namespace AvidScript;
        public readonly struct AvidCancellationToken {
            internal readonly long Value;
            internal AvidCancellationToken(long value) { Value = value; }
            [MethodImpl(MethodImplOptions.InternalCall)]
            public static extern implicit operator CancellationToken(AvidCancellationToken token);
        }
        public static class AvidContinuations { public static AvidDelayAwaitable NextTickAsync() => default; }
        public readonly struct AvidDelayAwaitable {
            public AvidDelayAwaitable WithCancellation(CancellationToken token) => default;
            public AvidDelayAwaiter GetAwaiter() => default;
        }
        public readonly struct AvidDelayAwaiter : ICriticalNotifyCompletion {
            public bool IsCompleted => false;
            public void GetResult() { }
            public void OnCompleted(Action continuation) { }
            public void UnsafeOnCompleted(Action continuation) { }
        }
        """;
}
