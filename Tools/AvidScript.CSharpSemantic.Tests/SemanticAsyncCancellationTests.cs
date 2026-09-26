using System;
using System.IO;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpSemantic;

internal static class SemanticAsyncCancellationTests
{
    private static int passed;
    public static int Run()
    {
        passed = 0;
        string source = File.ReadAllText(Path.GetFullPath("Fixtures/Phase66/AsyncCancellationFlow.cs"));
        SemanticDocument document = Analyze(source);
        Check(document.Succeeded && document.SchemaVersion == 44 && document.SemanticVersion == "1.53",
            "versioned cancellation projection: " + string.Join("; ", document.Diagnostics.Select(d => d.Code + ": " + d.Message)));
        Check(document.AsyncMethods.Count == 7 && document.RejectedAsyncExceptionFlows is null,
            "every source method must be projected");
        Check(SemanticAsyncInvocationValidator.IsValid(document)
            && SemanticAsyncScopeValidator.IsValid(document), "invocation and lexical contracts");
        byte[] serialized = SemanticSerializer.Serialize(document);
        Check(SemanticAsyncInvocationValidator.IsValid(SemanticSerializer.Deserialize(serialized))
            && serialized.SequenceEqual(SemanticSerializer.Serialize(Analyze(source))), "deterministic round trip");

        SemanticAsyncMethod inner = Method(document, ".InnerAsync(");
        Check(!SemanticAsyncCancellationPlanValidator.IsValid(null!, inner)
            && !SemanticAsyncCancellationPlanValidator.IsValid(document, null!),
            "null reader input must fail closed");
        Check(SemanticAsyncExceptionOwnerFlow.TryAnalyze(inner, out var states)
            && states.Values.Any(state => state.HasFlag(SemanticAsyncExceptionOwnerState.HandledCancellation))
            && inner.Segments.Any(segment => segment.Transfer?.Kind == SemanticAsyncMethod.EndCatchTransferKind),
            "handled cancellation has an explicit release edge");
        Check(inner.Segments.Where(segment => segment.AwaitSite is not null)
            .All(segment => states[segment.Ordinal] == SemanticAsyncExceptionOwnerState.Normal),
            "no exception owner survives into the next await");
        SemanticAsyncMethod nested = Method(document, ".NestedAsync(");
        Check(nested.ExceptionPlan!.ExceptionScopes!.Count == 2
            && nested.ExceptionPlan.ExceptionScopes.Count(scope => scope.ParentProtectedRegionOrdinal is not null) == 1
            && nested.Segments.Any(segment => segment.Transfer?.Kind == SemanticAsyncMethod.RethrowTransferKind),
            "nested rethrow retains an outer dispatch route");

        Reject(document with { SchemaVersion = 43, SemanticVersion = "1.52" }, "downgrade to cleanup-only contract");
        Reject(document with { SemanticVersion = "1.52" }, "mismatched version");
        ReplaceAndReject(document, inner with { ExceptionPlan = inner.ExceptionPlan! with
            { CancellationTypeId = "type:global::System.Exception" } }, "forged cancellation type");
        ReplaceAndReject(document, inner with { ExceptionPlan = inner.ExceptionPlan! with
            { ExceptionScopes = Array.Empty<SemanticAsyncExceptionScope>() } }, "missing dispatch provenance");
        ReplaceAndReject(document, inner with { ExceptionPlan = inner.ExceptionPlan! with
            { CancellationTypeId = null } }, "missing cancellation payload type");
        var innerScope = inner.ExceptionPlan!.ExceptionScopes![0];
        ReplaceAndReject(document, inner with { ExceptionPlan = inner.ExceptionPlan with
            { ExceptionScopes = new[] { innerScope with { UnwindTarget = innerScope.DispatchTarget } } } },
            "unwind must not re-enter sibling catches");
        var awaitSegment = inner.Segments.First(segment => segment.Transfer?.CancellationTarget is >= 0);
        MutateAndReject(document, inner, awaitSegment.Ordinal,
            awaitSegment.Transfer! with { CancellationTarget = awaitSegment.Transfer.PrimaryTarget },
            "cancellation cannot resume success code");
        MutateAndReject(document, inner, awaitSegment.Ordinal,
            awaitSegment.Transfer! with { CancellationTarget = inner.ExceptionPlan!.ExceptionScopes![0].UnwindTarget },
            "cancellation cannot skip matching handlers");
        var rethrow = nested.Segments.Single(segment => segment.Transfer?.Kind == SemanticAsyncMethod.RethrowTransferKind);
        MutateAndReject(document, nested, rethrow.Ordinal,
            rethrow.Transfer! with { Kind = SemanticAsyncMethod.EndCatchTransferKind }, "rethrow cannot discard its owner");
        ReplaceAndReject(document, inner with { Segments = inner.Segments.Select(segment =>
            segment.Transfer?.Kind == SemanticAsyncMethod.EndCatchTransferKind
                ? segment with { Transfer = segment.Transfer with { Kind = SemanticAsyncMethod.GotoTransferKind } }
                : segment).ToArray() }, "handled owner cannot leak into another await");
        var outer = Method(document, ".OuterAsync(");
        var taskAwait = outer.Segments.Single(segment => segment.AwaitSite is not null);
        Check(taskAwait.Transfer!.SecondaryTarget == taskAwait.Transfer.CancellationTarget,
            "Task cancellation and language faults enter the same ordered dispatch");
        var repeated = Method(document, ".RepeatedAsync(");
        Check(repeated.TaskLocalSymbolIds is { Count: 2 }
            && SemanticAsyncInvocationValidator.TryGetTaskLocalFlow(repeated,
                repeated.TaskLocalSymbolIds, true, out var producers, out var aliases,
                out var before, out var after)
            && aliases.Count == 1 && producers.Values.Distinct().Count() == 1
            && repeated.Segments.Where(segment => segment.Transfer!.Kind is
                SemanticAsyncMethod.CatchMatchTransferKind or SemanticAsyncMethod.EndCatchTransferKind
                or SemanticAsyncMethod.RethrowTransferKind or SemanticAsyncMethod.PropagateExceptionTransferKind)
                .All(segment => before[segment.Ordinal].Count == 2 && after[segment.Ordinal].Count == 2),
            "normal, handler and propagation paths preserve both local owners");
        ReplaceAndReject(document, repeated with { TaskLocalSymbolIds = null },
            "omitted Task alias ownership");
        string withoutAlias = source.Replace("Task<int> alias = pending;", "")
            .Replace("await alias", "await pending");
        var repeatedLocal = Analyze(withoutAlias);
        Check(repeatedLocal.Succeeded && SemanticAsyncInvocationValidator.IsValid(repeatedLocal)
            && Method(repeatedLocal, ".RepeatedAsync(").TaskLocalSymbolIds is null,
            "repeated await also works without an alias list");
        string conditional = source.Replace("Task<int> alias = pending;", "")
            .Replace("try { result = await pending; }",
                "try { result = await pending; Task<int> local = pending; result = await local; }")
            .Replace("await alias", "await pending");
        var conditionalLocal = Analyze(conditional);
        Check(conditionalLocal.Succeeded && SemanticAsyncInvocationValidator.IsValid(conditionalLocal),
            "a skipped late alias can merge at exception dispatch with guarded ownership");
        var branches = Method(document, ".ConditionalAsync(");
        Check(branches.CompilerLocals.Count == 0,
            "return outside try/finally must not retain an unreachable cleanup return slot");
        Check(SemanticAsyncTaskOwnership.TryAnalyze(branches, branches.TaskLocalSymbolIds!, true, out var ownership)
            && ownership!.RequiresGuards && ownership.PossibleAtEntry.Any(pair => pair.Value.Count == 5
                && ownership.DefiniteAtEntry[pair.Key].Count == 0),
            "branch join distinguishes possible owners from definitely initialized locals");
        string rightId = document.Symbols.Single(symbol => symbol.ContainingSymbolId == branches.MethodSymbolId
            && symbol.Name == "right").Id;
        var leftAwait = branches.Segments.First(segment => segment.AwaitSite?.ProducerKind == "task_local");
        var invalidRead = branches with { Segments = branches.Segments.Select(segment => segment == leftAwait
            ? segment with { AwaitSite = segment.AwaitSite! with { TaskLocalSymbolId = rightId,
                Arguments = new[] { segment.AwaitSite!.Arguments[0] with { SymbolId = rightId } } } }
            : segment).ToArray() };
        Check(!SemanticAsyncTaskOwnership.TryAnalyze(invalidRead, branches.TaskLocalSymbolIds!, true, out _),
            "a possibly present owner cannot authorize an await on a different branch");
        ReplaceAndReject(document, invalidRead, "conditional owner used without definite initialization");
        var copied = branches.Segments.Single(segment => segment.Statements.Any(statement =>
            statement.TargetSymbolId is { } id && document.Symbols.Any(symbol => symbol.Id == id && symbol.Name == "copied")));
        var alias = copied.Statements.Single();
        foreach (string sourceId in new[] { rightId, alias.TargetSymbolId! })
        {
            var invalidAlias = branches with { Segments = branches.Segments.Select(segment => segment == copied
                ? segment with { Statements = new[] { alias with { Operation = alias.Operation with { SymbolId = sourceId } } } }
                : segment).ToArray() };
            Check(!SemanticAsyncTaskOwnership.TryAnalyze(invalidAlias, branches.TaskLocalSymbolIds!, true, out _),
                "aliases must reject an uninitialized branch source or an ownership cycle");
        }
        var reentry = branches with { Segments = branches.Segments.Select(segment => segment == leftAwait
            ? segment with { Transfer = segment.Transfer! with { PrimaryTarget = copied.Ordinal } } : segment).ToArray() };
        Check(!SemanticAsyncTaskOwnership.TryAnalyze(reentry, branches.TaskLocalSymbolIds!, true, out _),
            "re-entering an owned declaration must not overwrite a live Task token");
        var scope = nested.ExceptionPlan!.ExceptionScopes!.Single(item => item.ParentProtectedRegionOrdinal is not null);
        ReplaceAndReject(document, nested with { ExceptionPlan = nested.ExceptionPlan with
            { ExceptionScopes = nested.ExceptionPlan.ExceptionScopes.Select(item => item == scope
                ? item with { ParentProtectedRegionOrdinal = null } : item).ToArray() } }, "missing nested cleanup parent");
        SemanticDocument legacy = Analyze(source, cancellation: false);
        Check(legacy.SchemaVersion != 44 && legacy.AsyncMethods.All(method =>
            method.ExceptionPlan?.CancellationTypeId is null), "legacy mode is unchanged");
        SemanticDocument unsupported = Analyze(source.Replace("catch (TaskCanceledException)",
            "catch (TaskCanceledException error)"));
        Check(!unsupported.Succeeded && unsupported.RejectedAsyncExceptionFlows is not null,
            "exception variables remain rejected until payload objects are exposed");
        foreach (string clause in new[] { "catch { }", "catch (System.Exception) { }" })
        {
            SemanticDocument emptyCatch = Analyze("using AvidScript; using System.Threading.Tasks; "
                + "public static class Script { public static async Task<int> Run() { "
                + "try { await AvidContinuations.NextTickAsync(); } " + clause
                + " await AvidContinuations.NextTickAsync(); return 2; } }");
            Check(emptyCatch.Succeeded && SemanticAsyncInvocationValidator.IsValid(emptyCatch)
                && SemanticAsyncScopeValidator.IsValid(emptyCatch),
                "empty handler must consume the cancellation owner: " + clause);
        }
        SemanticDocument cleanupOnly = Analyze("using AvidScript; using System.Threading.Tasks; "
            + "public static class Script { public static int Cleanup; public static async Task<int> Run() { "
            + "try { await AvidContinuations.NextTickAsync(); return 2; } finally { Cleanup++; } } }");
        Check(cleanupOnly.Succeeded && SemanticAsyncInvocationValidator.IsValid(cleanupOnly),
            "finally without catch retains typed cancellation propagation");
        return passed;
    }

    private static SemanticDocument Analyze(string source, bool cancellation = true) => SemanticAnalyzer.Analyze(source,
        "Scripts/AsyncCancellationFlow.cs", FrontendAnalyzer.Analyze(source, "Scripts/AsyncCancellationFlow.cs").Source.Sha256,
        new[] { new SemanticReferenceSource(Facade, "generated://Cancellation.cs", true) },
        new SemanticCompilerWorkspace(), enableAsyncExceptionFlow: true,
        enableDirectAwaitCleanup: true, enableAsyncCancellationFlow: cancellation);
    private static SemanticAsyncMethod Method(SemanticDocument document, string name) =>
        document.AsyncMethods.Single(method => method.MethodSymbolId.Contains(name, StringComparison.Ordinal));
    private static void MutateAndReject(SemanticDocument document, SemanticAsyncMethod method,
        int ordinal, SemanticAsyncControlTransfer transfer, string reason) =>
        ReplaceAndReject(document, method with { Segments = method.Segments.Select(segment =>
            segment.Ordinal == ordinal ? segment with { Transfer = transfer } : segment).ToArray() }, reason);
    private static void ReplaceAndReject(SemanticDocument document, SemanticAsyncMethod method, string reason) =>
        Reject(document with { AsyncMethods = document.AsyncMethods.Select(item =>
            item.MethodSymbolId == method.MethodSymbolId ? method : item).ToArray() }, reason);
    private static void Reject(SemanticDocument document, string reason) =>
        Check(!SemanticAsyncInvocationValidator.IsValid(document), reason);
    private static void Check(bool condition, string reason)
    {
        if (!condition) throw new InvalidOperationException(reason);
        passed++;
    }

    private const string Facade = """
        using System;
        using System.Runtime.CompilerServices;
        namespace AvidScript;
        public readonly struct AvidCancellationToken { public readonly long Value; }
        public readonly struct AvidCancellationSource { public AvidCancellationToken Token => default; }
        public static class AvidContinuations
        {
            public static AvidDelayAwaitable NextTickAsync() => default;
        }
        public readonly struct AvidDelayAwaitable
        {
            public AvidDelayAwaitable WithCancellation(AvidCancellationToken token) => this;
            public AvidDelayAwaiter GetAwaiter() => default;
        }
        public readonly struct AvidDelayAwaiter : ICriticalNotifyCompletion
        {
            public bool IsCompleted => false;
            public void GetResult() { }
            public void OnCompleted(Action continuation) { }
            public void UnsafeOnCompleted(Action continuation) { }
        }
        """;
}
