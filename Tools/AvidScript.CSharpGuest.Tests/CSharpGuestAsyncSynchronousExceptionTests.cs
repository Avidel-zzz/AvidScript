using System;
using System.Linq;
using System.Security.Cryptography;
using System.Text.Json;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestAsyncSynchronousExceptionTests
{
    public static int Run()
    {
        const string source = """
            using System; using System.Threading.Tasks; using System.Runtime.InteropServices;
            public static class Script {
                public static int Result;
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static async void BeginPlay() { Result = await Run(1); }
                public static int Read(int mode) { if (mode == 0) throw new ArgumentException(); return 7; }
                public static async Task<int> Run(int mode) { return Read(mode); }
            }
            """;
        const string sourceId = "Scripts/SynchronousExceptionPublication.cs";
        string hash = FrontendAnalyzer.Analyze(source, sourceId).Source.Sha256;
        var document = SemanticAnalyzer.Analyze(source, sourceId, hash,
            new[] { new SemanticReferenceSource(CSharpGuestContinuationTests.ReferenceFacade,
                "generated://AvidScript.Continuations.generated.cs", true) }, new SemanticCompilerWorkspace(),
            enableAsyncExceptionFlow: true, enableAsyncCancellationFlow: true,
            enableAsyncSynchronousExceptions: true);
        int count = 0;
        void Check(bool valid, string reason)
        {
            if (!valid) throw new InvalidOperationException(reason);
            count++;
        }
        Check(SemanticAsyncInvocationValidator.IsValid(document), "New source routes must validate before publication is attempted");
        foreach (bool enabled in new[] { false, true })
        foreach (var candidate in new[]
        {
            document,
            document with { SchemaVersion = 47, SemanticVersion = "1.56" },
            document with { SemanticVersion = "1.58" },
            document with { AsyncMethods = document.AsyncMethods.Select(method => method with
            {
                Segments = method.Segments.Select(segment => segment with { SynchronousExceptionTarget = null }).ToArray(),
            }).ToArray() },
        })
        {
            string semanticHash = Convert.ToHexString(SHA256.HashData(SemanticSerializer.Serialize(candidate))).ToLowerInvariant();
            var result = CSharpGuestLowerer.Lower(candidate, semanticHash, enableAsyncLanguageErrors: enabled);
            Check(!result.Succeeded && result.Module is null && result.Diagnostics.Any(item => item.Code == "ASCG1026"),
                "Unintegrated or disguised routes must never publish a Guest module");
        }
        string compiledHash = Convert.ToHexString(SHA256.HashData(SemanticSerializer.Serialize(document))).ToLowerInvariant();
        Check(CSharpLanguageErrorCompiler.TryLower(document, compiledHash, out var compiled, out string? compileError)
            && compiled is not null, "Source synchronous-to-Task composition failed: " + compileError);
        Check(compiled!.Module.SchemaVersion == 29 && compiled.Module.IrVersion == "1.28"
            && compiled.Module.Provenance.SemanticSchemaVersion == 50
            && compiled.Module.AsyncSynchronousExceptions is { Sites.Count: > 0 }
            && GuestModuleValidator.Validate(compiled.Module).Succeeded,
            "Source composition must retain its own IR and Semantic provenance with checked transfer sites");
        var wasm = WasmModuleCompiler.Compile(compiled.Module);
        Check(wasm.Succeeded, "Source synchronous exception WASM: " + string.Join(" | ", wasm.Diagnostics.Select(item => item.Message)));
        CheckSharedCatalog(Check);
        return count + CSharpGuestAsyncSynchronousExecutionTests.Run();
    }

    private static void CheckSharedCatalog(Action<bool, string> check)
    {
        const string source = """
            using System; using System.Threading.Tasks; using AvidScript;
            public static class Script {
                public static int Read() { throw new ArgumentException(); }
                public static int Other() { throw new ArgumentException(); }
                public static async Task<int> Run(int mode) {
                    try {
                        await AvidContinuations.NextTickAsync();
                        if (mode == 1) throw new InvalidOperationException();
                        return Read();
                    } catch (ArgumentException) { return 7; }
                }
            }
            """;
        const string sourceId = "Scripts/SharedExceptionCatalog.cs";
        SemanticDocument Analyze(bool composed) => SemanticAnalyzer.Analyze(source, sourceId,
            FrontendAnalyzer.Analyze(source, sourceId).Source.Sha256,
            new[] { new SemanticReferenceSource(CSharpGuestContinuationTests.ReferenceFacade,
                "generated://AvidScript.Continuations.generated.cs", true) },
            new SemanticCompilerWorkspace(), enableAsyncExceptionFlow: true, enableDirectAwaitCleanup: true,
            enableAsyncCancellationFlow: true, enableAsyncSynchronousExceptions: composed);
        var document = Analyze(true);
        check(SemanticContract.HasAsyncSynchronousExceptions(document)
            && SemanticExceptionFlowContractValidator.IsValid(document),
            "Mixed catalog input must retain synchronous flows and valid async routes: "
                + string.Join(" | ", document.Diagnostics.Select(item => item.Code + ":" + item.Message)));
        var sync = CSharpLanguageErrorCatalogBuilder.ForSynchronous(document, document.ExceptionFlows!);
        var asyncCatalog = CSharpAsyncLanguageErrorCatalog.Build(document);
        check(sync.Types.SequenceEqual(asyncCatalog.Types) && sync.Sources.SequenceEqual(asyncCatalog.Sources),
            "Synchronous producers and async handlers must see the same token identities");
        check(sync.Types.Select(item => (item.Token, item.TypeId)).SequenceEqual(new[]
        {
            (1, "type:global::System.ArgumentException"),
            (2, "type:global::System.InvalidOperationException"),
            (3, "type:global::System.Threading.Tasks.TaskCanceledException"),
        }), "A synchronous type sorted before async types must shift every consumer consistently");
        string[] fragments = sync.Sources.Select(item => source.Substring(item.Span.Start, item.Span.Length)).ToArray();
        check(fragments.Count(fragment => fragment.Contains("throw new ArgumentException", StringComparison.Ordinal)) == 2
            && fragments.Count(fragment => fragment.Contains("throw new InvalidOperationException", StringComparison.Ordinal)) == 1
            && fragments.Count(fragment => fragment.Contains("NextTickAsync", StringComparison.Ordinal)) == 1
            && sync.Sources.Select(item => item.Token).SequenceEqual(new[] { 1, 2, 3, 4 }),
            "Repeated types share a token while each synchronous throw, async throw and cancellation site keeps its own source");
        var method = document.AsyncMethods.Single();
        var throwSegment = method.Segments.Single(segment => segment.Transfer?.Kind == SemanticAsyncMethod.RaiseExceptionTransferKind);
        var throwRoute = CSharpAsyncThrowLowerer.Route(document, method, throwSegment);
        check(throwRoute.TypeToken == 2 && throwRoute.SourceToken == sync.Sources.Single(item =>
            source.Substring(item.Span.Start, item.Span.Length).Contains("throw new InvalidOperationException", StringComparison.Ordinal)).Token,
            "The actual async throw emitter must consume the combined catalog");
        var awaitSite = method.Segments.Single(segment => segment.AwaitSite is not null).AwaitSite!;
        var cancelRoute = CSharpAsyncCancellationLowerer.Route(document, method, awaitSite);
        check(cancelRoute.TypeToken == 3 && cancelRoute.SourceToken == sync.Sources.Single(item =>
            source.Substring(item.Span.Start, item.Span.Length).Contains("NextTickAsync", StringComparison.Ordinal)).Token,
            "The actual cancellation emitter must consume the combined catalog");
        var shuffled = document with { ExceptionFlows = document.ExceptionFlows!.Reverse().ToArray(),
            AsyncMethods = document.AsyncMethods.Reverse().ToArray() };
        var repeated = CSharpLanguageErrorCatalogBuilder.ForSynchronous(shuffled, shuffled.ExceptionFlows!);
        check(sync.Types.SequenceEqual(repeated.Types) && sync.Sources.SequenceEqual(repeated.Sources),
            "Catalog numbering must not depend on analysis collection order");
        var restored = SemanticSerializer.Deserialize(SemanticSerializer.Serialize(document));
        var roundTrip = CSharpAsyncLanguageErrorCatalog.Build(restored);
        check(sync.Types.SequenceEqual(roundTrip.Types) && sync.Sources.SequenceEqual(roundTrip.Sources),
            "Source contract round-trip must retain cross-boundary token identities");

        // Static guards already participate in the same synchronous producer
        // catalog. Their cached wrapper must not collide with a source exception.
        var staticContext = new CSharpStaticExecutionContext(Array.Empty<CSharpStaticField>(), new[]
        {
            new CSharpStaticSourceType("type:test:StaticOwner", false, "test:cctor", sourceId,
                source.Length, document.ExceptionFlows![0].Throws[0].Span),
        });
        staticContext.Attach(document);
        var withStatic = CSharpLanguageErrorCatalogBuilder.ForSynchronous(document, document.ExceptionFlows!);
        var asyncWithStatic = CSharpAsyncLanguageErrorCatalog.Build(document);
        check(withStatic.Types.Count == 4 && withStatic.Types[^1].TypeId == "type:global::System.TypeInitializationException"
            && withStatic.Types[^1].Token == 4 && withStatic.Sources.Count == sync.Sources.Count
            && withStatic.Types.SequenceEqual(asyncWithStatic.Types) && withStatic.Sources.SequenceEqual(asyncWithStatic.Sources),
            "Static wrapper numbering must share the catalog without duplicating a source location");
        check(!CSharpGuestLowerer.Lower(document, new string('a', 64), enableAsyncLanguageErrors: true).Succeeded,
            "A transient catalog cannot authorize an unintegrated execution contract");

        var legacy = Analyze(false);
        var oldSync = CSharpLanguageErrorCatalogBuilder.ForSynchronous(legacy, legacy.ExceptionFlows!);
        var oldAsync = CSharpAsyncLanguageErrorCatalog.Build(legacy);
        check(oldSync.Types.Count == 1 && oldSync.Types[0] == new CSharpLanguageErrorTypeToken(1, "type:global::System.ArgumentException")
            && oldAsync.Types.Select(item => (item.Token, item.TypeId)).SequenceEqual(new[]
            {
                (1, "type:global::System.InvalidOperationException"),
                (2, "type:global::System.Threading.Tasks.TaskCanceledException"),
            }), "Old schemas must retain independent token tables rather than changing published identifiers");
        check(CSharpLanguageErrorCatalogBuilder.TryToGuest(legacy, oldAsync, out var guest, out _)
            && CSharpLanguageErrorCatalogBuilder.TryToGuest(Analyze(false), oldAsync, out var repeatedGuest, out _)
            && JsonSerializer.Serialize(guest) == JsonSerializer.Serialize(repeatedGuest),
            "Legacy catalog serialization must remain deterministic");
        check(CSharpLanguageErrorCatalogBuilder.TryToGuest(document, withStatic, out var combinedGuest, out _)
            && combinedGuest!.Types.Count == 4 && combinedGuest.Sources.All(site => site.SourceLength == source.Length),
            "Composed publication must bind all catalog entries to a verified source length");
        var flow = document.ExceptionFlows![0];
        var wrongLength = document with { ExceptionFlows = document.ExceptionFlows.Select(item =>
            item == flow ? item with { SourceLength = flow.SourceLength + 1 } : item).ToArray() };
        check(!CSharpLanguageErrorCatalogBuilder.TryToGuest(wrongLength, sync, out _, out _),
            "A shared source ID with contradictory lengths must be rejected");
        var asyncWrongLength = document with { AsyncMethods = new[] { method with
            { ErrorPlan = method.ErrorPlan! with { SourceLength = method.ErrorPlan!.SourceLength + 1 } } } };
        check(!CSharpLanguageErrorCatalogBuilder.TryToGuest(asyncWrongLength, sync, out _, out _),
            "An async source identity must not silently override a synchronous source identity");
        foreach (var brokenSite in new[]
        {
            sync.Sources[0] with { SourceId = "Scripts/Missing.cs" },
            sync.Sources[0] with { Span = sync.Sources[0].Span with { Start = source.Length } },
            sync.Sources[0] with { Span = sync.Sources[0].Span with { Start = -1 } },
            sync.Sources[0] with { Span = sync.Sources[0].Span with { Length = int.MaxValue } },
        }) check(!CSharpLanguageErrorCatalogBuilder.TryToGuest(document, sync with { Sources = new[] { brokenSite } }, out _, out _),
            "Unmapped or out-of-range source locations must not enter the runtime catalog");
        var otherUnit = document with { ExceptionFlows = document.ExceptionFlows.Select(item =>
            item == flow ? item with { SourceId = "Scripts/Other.cs", SourceLength = source.Length + 100 } : item).ToArray() };
        var otherCatalog = CSharpLanguageErrorCatalogBuilder.ForSynchronous(otherUnit, otherUnit.ExceptionFlows!);
        check(CSharpLanguageErrorCatalogBuilder.TryToGuest(otherUnit, otherCatalog, out var multiUnit, out _)
            && multiUnit!.Sources.Single(site => site.SourceId == "Scripts/Other.cs").SourceLength == source.Length + 100
            && multiUnit.Sources.Where(site => site.SourceId == sourceId).All(site => site.SourceLength == source.Length),
            "Different source units must retain their own lengths instead of inheriting the primary file length");
    }
}
