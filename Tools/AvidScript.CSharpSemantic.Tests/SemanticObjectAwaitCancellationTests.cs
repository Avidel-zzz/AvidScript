using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpSemantic;

internal static class SemanticObjectAwaitCancellationTests
{
    public static int Run()
    {
        int count = 0;
        void Check(bool condition, string reason)
        {
            if (!condition) throw new InvalidOperationException("Object await cancellation Semantic: " + reason);
            count++;
        }
        bool Valid(SemanticDocument document) =>
            SemanticComposableCapabilityValidator.IsValid(document)
            && SemanticObjectAwaitCancellationValidator.IsValid(document)
            && (document.StaticInitialization is null || SemanticStaticInitializationValidator.IsValid(document))
            && SemanticAsyncInvocationValidator.IsValid(document)
            && SemanticAsyncScopeValidator.IsValid(document);

        foreach (bool staticState in new[] { false, true })
        foreach (bool token in new[] { false, true })
        foreach (bool namedCatch in new[] { false, true })
        {
            string source = Source(staticState, token, namedCatch);
            SemanticDocument document = Analyze(source);
            string shape = $"static={staticState}, token={token}, catch={namedCatch}";
            Check(SemanticObjectAwaitCancellation.IsVersion(document), shape + " owns Semantic 58/1.67: " + Describe(document));
            Check(Valid(document), shape + " all source contracts validate: " + Describe(document));
            var manifest = document.CapabilityManifest!;
            Check(manifest.BaseSchemaVersion == (token ? SemanticContract.CancellationTokenSchemaVersion
                    : namedCatch ? SemanticContract.AsyncCatchVariableSchemaVersion
                    : SemanticContract.AsyncSynchronousExceptionSchemaVersion)
                && manifest.BaseSemanticVersion == (token ? SemanticContract.CancellationTokenSemanticVersion
                    : namedCatch ? SemanticContract.AsyncCatchVariableSemanticVersion
                    : SemanticContract.AsyncSynchronousExceptionSemanticVersion), shape + " exact source base");
            Check(SemanticComposableCapabilities.Has(document, SemanticComposableCapabilities.StaticStorage) == staticState
                && SemanticContract.HasCancellationTokens(document) == token
                && !SemanticContract.HasAsyncVoidErrorOwner(document), shape + " only source-backed optional capabilities");
            byte[] bytes = SemanticSerializer.Serialize(document);
            Check(bytes.SequenceEqual(SemanticSerializer.Serialize(Analyze(source)))
                && bytes.SequenceEqual(SemanticSerializer.Serialize(SemanticSerializer.Deserialize(bytes)))
                && Valid(SemanticSerializer.Deserialize(bytes)), shape + " deterministic canonical round trip");
            Check(document.AsyncMethods.SelectMany(method => method.Segments)
                .Where(segment => segment.AwaitSite?.ProducerKind == SemanticObjectAwaitCancellation.ProducerKind)
                .All(segment => segment.Transfer?.CancellationTarget is >= 0
                    && segment.Transfer.SecondaryTarget == -1
                    && segment.Transfer.CancellationTarget != segment.Transfer.PrimaryTarget), shape + " cancellation has a separate cleanup route");
        }

        SemanticDocument baseline = Analyze(Source(true, true, true));
        SemanticDocument MutateSite(Func<SemanticAsyncAwaitSite, SemanticAsyncAwaitSite> mutate) => baseline with {
            AsyncMethods = baseline.AsyncMethods.Select(method => method with {
                Segments = method.Segments.Select(segment => segment.AwaitSite is { ProducerKind: "object_load" } site
                    ? segment with { AwaitSite = mutate(site) } : segment).ToArray(),
            }).ToArray(),
        };
        Check(!Valid(MutateSite(site => site with { PayloadKind = SemanticContinuationCallback.NonePayloadKind })), "object producer cannot use a timer payload");
        Check(!Valid(MutateSite(site => site with { BindingOrdinal = 0 })), "built-in object producer cannot claim a latent binding");
        Check(!Valid(MutateSite(site => site with { TaskCallableId = "symbol:forged" })), "object producer cannot borrow Task identity");
        Check(!Valid(MutateSite(site => site with { CallbackId = 0 })), "invalid callback identity rejects");
        Check(!Valid(MutateSite(site => site with { CallbackId = 1 })), "compiler await cannot take an explicit user callback identity");
        Check(!Valid(MutateSite(site => site with { ResultTypeId = "type:int32" })), "object local has exact result type");
        Check(!Valid(baseline with { Types = baseline.Types.Select(type => type.CanonicalName == SemanticObjectAwaitCancellation.LoadedObjectTypeName
            ? type with { IsNullable = true } : type).ToArray() }), "object payload cannot acquire CLR nullable layout");
        Check(!Valid(MutateSite(site => site with { ResultStorageKind = "static_field" })), "object payload cannot bypass staged local result ownership");
        Check(!Valid(MutateSite(site => site with { Arguments = Array.Empty<SemanticOperation>() })), "object producer requires its single path argument");
        Check(!Valid(MutateSite(site => site with { Arguments = new[] { site.Arguments[0] with {
            Constant = new SemanticConstant("string", "/Engine//Invalid.Asset") } } })), "noncanonical path rejects in the independent reader");
        Check(!Valid(baseline with { Types = baseline.Types.Append(baseline.Types[0]).ToArray() }), "duplicate type identities reject without an exception");
        Check(!Valid(baseline with { Symbols = baseline.Symbols.Append(baseline.Symbols[0]).ToArray() }), "duplicate symbol identities reject");
        var plan = baseline.CapabilityManifest!;
        Check(!Valid(baseline with { CapabilityManifest = null }), "source schema alone grants no capability");
        Check(!Valid(baseline with { CapabilityManifest = plan with { Capabilities = plan.Capabilities
            .Where(capability => capability.Id != SemanticObjectAwaitCancellation.CapabilityId).ToArray() } }), "missing object capability rejects");
        Check(!Valid(baseline with { CapabilityManifest = plan with { BaseSchemaVersion = SemanticContract.AsyncCatchVariableSchemaVersion,
            BaseSemanticVersion = SemanticContract.AsyncCatchVariableSemanticVersion } }), "token source cannot claim the catch-only base");
        Check(!Valid(baseline with { SchemaVersion = SemanticObjectAwaitCancellation.SchemaVersion + 1 })
            && !Valid(baseline with { SemanticVersion = "1.68" }), "future and mismatched version pairs reject");
        Check(!Valid(baseline with { SchemaVersion = SemanticComposableCapabilities.StaticAsyncValueSchemaVersion,
            SemanticVersion = SemanticComposableCapabilities.StaticAsyncValueSemanticVersion,
            CapabilityManifest = plan with { Capabilities = plan.Capabilities
                .Where(capability => capability.Id != SemanticObjectAwaitCancellation.CapabilityId).ToArray() } }), "older composition cannot inherit object cancellation routes");
        Check(!Valid(baseline with { AsyncMethods = baseline.AsyncMethods.Select(method => method with {
            Segments = method.Segments.Select(segment => segment.AwaitSite?.ProducerKind == "object_load"
                ? segment with { Transfer = segment.Transfer! with { CancellationTarget = segment.Transfer.PrimaryTarget } }
                : segment).ToArray() }).ToArray() }), "cancellation cannot write back through the normal result branch");
        Check(!Valid(baseline with { AsyncMethods = baseline.AsyncMethods.Select(method => method with {
            Segments = method.Segments.Select(segment => segment.AwaitSite?.ProducerKind == "object_load"
                ? segment with { Transfer = segment.Transfer! with { CancellationTarget = null } }
                : segment).ToArray() }).ToArray() }), "protected object await cannot lose cleanup");
        Check(!Valid(baseline with { AsyncMethods = baseline.AsyncMethods.Select(method => method with {
            ExceptionPlan = method.ExceptionPlan! with { CancellationTypeId = "type:global::System.Exception" } }).ToArray() }), "cancellation retains its exact language type");
        Check(!Valid(MutateSite(site => site with { StateFrame = null })), "object await cannot drop the captured token needed by catch");
        Check(!Valid(MutateSite(site => site with { StateFrame = site.StateFrame! with {
            Slots = site.StateFrame.Slots.Select(slot => slot with { TypeId = "type:int32" }).ToArray() } })), "state-frame slot types remain exact");
        Check(!Valid(baseline with { StaticInitialization = baseline.StaticInitialization! with {
            BaseSchemaVersion = SemanticContract.AsyncSynchronousExceptionSchemaVersion,
            BaseSemanticVersion = SemanticContract.AsyncSynchronousExceptionSemanticVersion } }), "static plan cannot downgrade the source base");

        SemanticDocument implicitCancellation = Analyze("""
            using System.Threading.Tasks; using AvidScript;
            public static class Script {
                public static async Task<int> Run() {
                    await AvidAssets.LoadObjectAsync("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture");
                    return 3;
                }
            }
            """);
        Check(Valid(implicitCancellation) && SemanticObjectAwaitCancellation.Has(implicitCancellation)
            && implicitCancellation.AsyncMethods.Single().Segments.Single(segment => segment.AwaitSite is not null)
                .Transfer!.CancellationTarget is null, "unprotected Task await preserves implicit cancellation propagation");
        Check(!Valid(implicitCancellation with { AsyncMethods = implicitCancellation.AsyncMethods.Select(method => method with {
            Segments = method.Segments.Select(segment => segment.AwaitSite is not null
                ? segment with { AwaitSite = segment.AwaitSite with { ProducerKind = "next_tick",
                    PayloadKind = SemanticContinuationCallback.NonePayloadKind, Arguments = Array.Empty<SemanticOperation>() } }
                : segment).ToArray() }).ToArray() }), "source cannot claim an object capability after replacing every producer");
        SemanticDocument old = Analyze("""
            using System.Threading.Tasks; using AvidScript;
            public static class Script {
                public static async Task<int> Run() {
                    await AvidAssets.LoadObjectAsync("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"); return 3;
                }
            }
            """, objectCancellation: false);
        Check(old.SchemaVersion == SemanticContract.AsyncSynchronousExceptionSchemaVersion
            && old.CapabilityManifest is null && !SemanticObjectAwaitCancellation.Has(old)
            && SemanticObjectAwaitCancellationValidator.IsValid(old), "previous profile keeps its original source contract");

        SemanticDocument voidOwner = Analyze(Source(true, true, true).Replace(
            "async Task<int> Run(CancellationToken token)", "async void Run(CancellationToken token)", StringComparison.Ordinal)
            .Replace("return 1;", "return;", StringComparison.Ordinal)
            .Replace("return error.CancellationToken == token ? 7 : 9;", "State = error.CancellationToken == token ? 7 : 9;", StringComparison.Ordinal), voidOwner: true);
        Check(Valid(voidOwner) && SemanticContract.HasAsyncVoidErrorOwner(voidOwner)
            && voidOwner.CapabilityManifest!.BaseSchemaVersion == SemanticContract.AsyncVoidErrorOwnerSchemaVersion,
            "async void retains its private error owner in object composition: " + Describe(voidOwner));

        SemanticDocument genericState = Analyze(Source(true, true, true)
            .Replace("private static int State = 1;", "", StringComparison.Ordinal)
            .Replace("State = State + 1;", "Cache<int>.State = Cache<int>.State + 1; Cache<long>.State = Cache<long>.State + 2;", StringComparison.Ordinal)
            + " public static class Cache<T> { public static int State = 1; }");
        Check(Valid(genericState) && genericState.AsyncMethods.SelectMany(method => method.Segments)
            .SelectMany(segment => segment.Statements).SelectMany(statement => Nodes(statement.Operation))
            .Where(operation => operation.Kind == "field_reference").Select(operation => operation.StaticFieldOwnerTypeId)
            .ToHashSet(StringComparer.Ordinal).SetEquals(new[] { "type:global::Cache<int>", "type:global::Cache<long>" }),
            "constructed generic static owners remain separate with object cancellation: " + Describe(genericState));
        SemanticDocument usedObject = Analyze(Source(false, false, false)
            .Replace("return 1;", "return loaded.Slot;", StringComparison.Ordinal));
        Check(Valid(usedObject), "normal source can consume its exact loaded-object result: " + Describe(usedObject));
        SemanticDocument twoObjects = Analyze(Source(false, false, false).Replace("return 1;",
            "var second = await AvidAssets.LoadObjectAsync(\"/Engine/EngineResources/DefaultTexture.DefaultTexture\"); return 1;", StringComparison.Ordinal));
        Check(Valid(twoObjects), "sequential object awaits validate independently: " + Describe(twoObjects));
        int firstCallback = twoObjects.AsyncMethods.Single().Segments.First(segment => segment.AwaitSite is not null).AwaitSite!.CallbackId;
        Check(!Valid(twoObjects with { AsyncMethods = twoObjects.AsyncMethods.Select(method => method with {
            Segments = method.Segments.Select(segment => segment.AwaitSite is { } site
                ? segment with { AwaitSite = site with { CallbackId = firstCallback } } : segment).ToArray() }).ToArray() }),
            "two object awaits cannot share callback identity");
        SemanticDocument nested = Analyze("""
            using System; using System.Threading; using System.Threading.Tasks; using AvidScript;
            public static class Script {
                static int State = 0;
                public static async Task<int> Run(CancellationToken token) {
                    try {
                        try { await AvidAssets.LoadObjectAsync("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture").WithCancellation(token); }
                        finally { State = State + 1; }
                        return State;
                    }
                    catch (OperationCanceledException error) { return error.CancellationToken == token ? 7 : 9; }
                    finally { State = State + 2; }
                }
            }
            """);
        Check(Valid(nested) && nested.AsyncMethods.Single().ExceptionPlan!.ExceptionScopes!.Count == 2,
            "nested cleanup keeps source-owned scope dispatch: " + Describe(nested));

        string json = Encoding.UTF8.GetString(SemanticSerializer.Serialize(baseline));
        bool RejectJson(string artifact)
        {
            try { SemanticSerializer.Deserialize(Encoding.UTF8.GetBytes(artifact)); return false; }
            catch (InvalidDataException) { return true; }
        }
        Check(RejectJson(json.Replace("\"schema_version\": 58,", "\"schema_version\": 58,\n  \"schema_version\": 58,", StringComparison.Ordinal))
            && RejectJson(json.Replace("\"capability_manifest\":", "\"unowned_flag\": true,\n  \"capability_manifest\":", StringComparison.Ordinal)), "duplicate and unknown serialized fields reject");
        return count;
    }

    private static SemanticDocument Analyze(string source, bool objectCancellation = true, bool voidOwner = false) =>
        SemanticAnalyzer.Analyze(source, "Scripts/ObjectAwait.cs",
            FrontendAnalyzer.Analyze(source, "Scripts/ObjectAwait.cs").Source.Sha256,
            new[] { new SemanticReferenceSource(Facade, "generated://ObjectAwait.cs", true) },
            new SemanticCompilerWorkspace(), enableAsyncExceptionFlow: true,
            enableDirectAwaitCleanup: objectCancellation, enableAsyncCancellationFlow: true,
            enableStaticInitialization: true, enableAsyncSynchronousExceptions: true,
            enableAsyncCatchVariables: true, enableCancellationTokens: true,
            enableAsyncVoidErrorOwner: voidOwner);

    private static string Source(bool staticState, bool token, bool namedCatch) =>
        "using System; using System.Threading; using System.Threading.Tasks; using AvidScript; public static class Script { "
        + (staticState ? "private static int State = 1; " : "")
        + "public static async Task<int> Run(" + (token ? "CancellationToken token" : "") + ") { "
        + "try { var loaded = await AvidAssets.LoadObjectAsync(\"/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture\")"
        + (token ? ".WithCancellation(token)" : "") + "; return 1; } "
        + "catch (OperationCanceledException" + (namedCatch ? " error" : "") + ") { "
        + (namedCatch && token ? "return error.CancellationToken == token ? 7 : 9;" : "return 7;")
        + " } finally { " + (staticState ? "State = State + 1;" : "int cleanup = 1;") + " } } }";

    private static string Describe(SemanticDocument document) => $"schema={document.SchemaVersion}/{document.SemanticVersion}; "
        + $"cap={SemanticComposableCapabilityValidator.IsValid(document)}, object={SemanticObjectAwaitCancellationValidator.IsValid(document)}, "
        + $"static={SemanticStaticInitializationValidator.IsValid(document)}, invocation={SemanticAsyncInvocationValidator.IsValid(document)}, "
        + $"scope={SemanticAsyncScopeValidator.IsValid(document)}; "
        + $"exception={SemanticAsyncExceptionPlanValidator.IsValid(document)}, sync={SemanticAsyncSynchronousExceptionValidator.IsValid(document)}, "
        + $"catch={SemanticAsyncCatchVariableValidator.IsValid(document)}, token={SemanticCancellationTokenValidator.IsValid(document)}, "
        + $"lifetime={SemanticAsyncTaskLocalLifetimeValidator.IsValid(document)}; "
        + string.Join(" | ", document.Diagnostics.Select(diagnostic => diagnostic.Code + ": " + diagnostic.Message));

    private static IEnumerable<SemanticOperation> Nodes(SemanticOperation operation)
    {
        yield return operation;
        foreach (SemanticOperation child in operation.Children)
            foreach (SemanticOperation node in Nodes(child)) yield return node;
    }

    // Reader/projection fixture. Production facade and two-VM execution are a
    // separate C18 dependency batch and are not claimed by these source tests.
    private const string Facade = """
        using System; using System.Runtime.CompilerServices; using System.Threading;
        namespace AvidScript;
        public readonly struct AvidCancellationToken {
            internal readonly long Value;
            internal AvidCancellationToken(long value) { Value = value; }
            [MethodImpl(MethodImplOptions.InternalCall)]
            public static extern implicit operator CancellationToken(AvidCancellationToken token);
        }
        public readonly struct AvidLoadedObject { public readonly int Slot; public readonly int Generation; }
        public static class AvidAssets { public static AvidObjectAwaitable LoadObjectAsync(string path) => default; }
        public readonly struct AvidObjectAwaitable {
            public AvidObjectAwaitable WithCancellation(CancellationToken token) => default;
            public AvidObjectAwaiter GetAwaiter() => default;
        }
        public readonly struct AvidObjectAwaiter : ICriticalNotifyCompletion {
            public bool IsCompleted => false;
            public AvidLoadedObject GetResult() => default;
            public void OnCompleted(Action continuation) { }
            public void UnsafeOnCompleted(Action continuation) { }
        }
        """;
}
