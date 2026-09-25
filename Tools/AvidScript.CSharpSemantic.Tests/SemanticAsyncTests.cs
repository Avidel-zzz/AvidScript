using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpSemantic;

internal static class SemanticAsyncTests
{
    private const int CompilerCallbackIdStart = 0x40000000;

    public static int Run()
    {
        SequentialAwaitsProjectStableSegments();
        EarlyReturnGuardsProjectStableControlOperations();
        CrossBoundaryLocalsPublishStateFrames();
        StructuredFlowProjectsExactStateFrames();
        NestedAwaitsProjectContinuationCfg();
        SwitchAwaitsProjectContinuationCfg();
        ArrayForeachAwaitsProjectCompilerState();
        NonFrozenAsyncShapesRemainFailClosed();
        CallbackRangesAndAwaitLimitsAreEnforced();
        ControlFlowSegmentLimitFailsClosed();
        GeneratedLatentProducerProjectsImportIdentity();
        LocalInitializersPreserveContextualConversions();
        TaskIntResultProjectsTypedReturn();
        TaskIntAwaitPublishesDirectTarget();
        TaskIntAwaitProjectsValueArguments();
        TaskIntLocalPublishesProducerAndFrame();
        TaskIntParallelLocalsPreserveBothOwners();
        TaskIntAliasesPreserveEveryOwner();
        TaskIntStaticFieldAssignmentIsVersionedAndBounded();
        TaskIntExistingLocalAssignmentPreservesStorage();
        TaskIntSuspendedCleanupFailsClosed();
        AwaitFailureSuccessorKeepsCleanupLocalAlive();
        RejectedAsyncExceptionRetainsRoslynRegions();
        SuspendedFinallyPreviewRoutesTaskFailureThroughCleanup();
        AsyncCatchPreviewPreservesHandlerAndCleanupRoutes();
        NestedSuspendedCleanupBindsDistinctRoslynRegions();
        TaskAndExceptionPlansKeepBothContracts();
        TaskIntThrowPublishesVersionedErrorPlan();
        return 28;
    }

    private static void NestedSuspendedCleanupBindsDistinctRoslynRegions()
    {
        const string source = """
            using AvidScript;
            using System.Threading.Tasks;
            public static class Script
            {
                public static int InnerCount;
                public static int OuterCount;
                public static async Task<int> LoadAsync()
                {
                    await AvidContinuations.NextTickAsync();
                    return 12;
                }
                public static async Task<int> RunAsync()
                {
                    try
                    {
                        try { int value = await LoadAsync(); return value; }
                        finally { InnerCount++; }
                    }
                    finally { OuterCount++; }
                }
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/NestedAsyncFinallyRegions.cs");
        SemanticExceptionFlow? flow = document.RejectedAsyncExceptionFlows
            ?.SingleOrDefault(item => item.MethodSymbolId.Contains(".RunAsync(",
                StringComparison.Ordinal));
        SemanticAsyncExceptionPreview? preview = flow?.AsyncContinuationPreview;
        Assert(!document.Succeeded && preview is { Regions.Count: 4 }
            && preview.Regions.Count(region => region.Kind == "try") == 2
            && preview.Regions.Count(region => region.Kind == "finally") == 2,
            "nested suspended cleanup must bind both source try/finally pairs");
        SemanticAsyncExceptionPreviewRegion inner = preview!.Regions.Single(region =>
            region.Kind == "finally" && region.SourceSpan.Start ==
                source.IndexOf("finally { InnerCount", StringComparison.Ordinal));
        SemanticAsyncExceptionPreviewRegion outer = preview.Regions.Single(region =>
            region.Kind == "finally" && region.SourceSpan.Start ==
                source.IndexOf("finally { OuterCount", StringComparison.Ordinal));
        Assert(inner.RoslynRegionOrdinal != outer.RoslynRegionOrdinal
            && flow!.Regions[inner.RoslynRegionOrdinal].Kind == "finally"
            && flow.Regions[outer.RoslynRegionOrdinal].Kind == "finally",
            "copied cleanup segments must retain distinct Roslyn region identities");
        int cursor = preview.Segments.Single(segment => segment.AwaitSite?.ProducerKind
            == "task_call").Transfer!.SecondaryTarget;
        List<int> cleanupOrder = new();
        HashSet<int> visited = new();
        while (visited.Add(cursor))
        {
            if (inner.Segments.Contains(cursor)) cleanupOrder.Add(inner.RoslynRegionOrdinal);
            if (outer.Segments.Contains(cursor)) cleanupOrder.Add(outer.RoslynRegionOrdinal);
            SemanticAsyncControlTransfer transfer = preview.Segments[cursor].Transfer!;
            if (transfer.Kind == SemanticAsyncMethod.PropagateFaultTransferKind) break;
            Assert(transfer.Kind == SemanticAsyncMethod.GotoTransferKind,
                "nested fault cleanup path must use deterministic goto edges");
            cursor = transfer.PrimaryTarget;
        }
        Assert(cleanupOrder.SequenceEqual(new[] { inner.RoslynRegionOrdinal,
                outer.RoslynRegionOrdinal })
            && preview.Segments[cursor].Transfer?.Kind
                == SemanticAsyncMethod.PropagateFaultTransferKind,
            "fault propagation must execute inner then outer finally exactly once");
    }

    private static void AsyncCatchPreviewPreservesHandlerAndCleanupRoutes()
    {
        const string source = """
            using AvidScript;
            using System;
            using System.Threading.Tasks;
            public static class Script
            {
                public static int CleanupCount;
                public static async Task<int> LoadAsync(int mode)
                {
                    await AvidContinuations.NextTickAsync();
                    if (mode == 1) throw new InvalidOperationException();
                    return 12;
                }
                public static async Task<int> RunAsync(int mode)
                {
                    int fallback = mode + 7;
                    int cleanupIncrement = mode + 1;
                    try
                    {
                        int value = await LoadAsync(mode);
                        return value;
                    }
                    catch (InvalidOperationException)
                    {
                        if (mode == 2) throw;
                        return fallback;
                    }
                    finally
                    {
                        CleanupCount = CleanupCount + cleanupIncrement;
                        if (mode == 3) throw new ArgumentException();
                    }
                }
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/AsyncCatchFaultPreview.cs");
        SemanticExceptionFlow? flow = document.RejectedAsyncExceptionFlows
            ?.SingleOrDefault(item => item.MethodSymbolId.Contains(".RunAsync(",
                StringComparison.Ordinal));
        SemanticAsyncExceptionPreview? preview = flow?.AsyncContinuationPreview;
        Assert(!document.Succeeded && preview is not null
            && document.Diagnostics.Any(item => item.Code == "ASCS5420")
            && document.ExceptionFlows is null
            && flow!.Catches.Single().ExceptionTypeId
                == "type:global::System.InvalidOperationException",
            "typed async catch/finally remains diagnostic until its execution contract exists");
        SemanticAsyncSegment awaited = preview!.Segments.Single(segment =>
            segment.AwaitSite?.ProducerKind == "task_call");
        SemanticAsyncSegment dispatch = preview.Segments[awaited.Transfer!.SecondaryTarget];
        Assert(preview.Regions.Count == 3
            && preview.Regions.Single(region => region.Kind == "try")
                .Segments.Contains(awaited.Ordinal)
            && preview.Regions.Single(region => region.Kind == "catch")
                .RoslynRegionOrdinal == flow!.Catches.Single().RegionOrdinal
            && preview.Regions.Single(region => region.Kind == "finally")
                .Segments.Count > 1,
            "protected await, typed catch and copied cleanup must bind their Roslyn regions");
        Assert(dispatch.Transfer is
            {
                Kind: SemanticAsyncMethod.CatchMatchTransferKind,
                ExceptionTypeId: "type:global::System.InvalidOperationException",
                PrimaryTarget: >= 0,
                SecondaryTarget: >= 0,
            }
            && dispatch.Transfer.PrimaryTarget != dispatch.Transfer.SecondaryTarget
            && preview.Segments.Any(segment => segment.Transfer?.Kind
                == SemanticAsyncMethod.PropagateFaultTransferKind)
            && preview.Segments.Any(segment => segment.Transfer is
                {
                    Kind: SemanticAsyncMethod.ThrowTransferKind,
                    Condition.TypeId: "type:global::System.ArgumentException",
            }),
            "await failure must enter a typed handler decision with unmatched propagation and replacing finally throw");
        string fallbackId = document.Symbols.Single(symbol => symbol.Kind == "local"
            && symbol.Name == "fallback" && symbol.ContainingSymbolId == flow!.MethodSymbolId).Id;
        string cleanupId = document.Symbols.Single(symbol => symbol.Kind == "local"
            && symbol.Name == "cleanupIncrement" && symbol.ContainingSymbolId == flow!.MethodSymbolId).Id;
        string modeId = document.Symbols.Single(symbol => symbol.Kind == "parameter"
            && symbol.Name == "mode" && symbol.ContainingSymbolId == flow!.MethodSymbolId).Id;
        string[] savedIds = awaited.AwaitSite!.StateFrame?.Slots
            .Select(slot => slot.SymbolId).ToArray() ?? Array.Empty<string>();
        Assert(savedIds.Contains(fallbackId) && savedIds.Contains(cleanupId)
            && savedIds.Contains(modeId),
            "catch-only, cleanup-only, and invocation input values must survive a suspended await");
        int cleanupStart = source.IndexOf("CleanupCount =", StringComparison.Ordinal);
        bool ReachesAfterCleanup(int start, string terminalKind)
        {
            Queue<(int Ordinal, bool Cleaned)> pending = new();
            HashSet<(int Ordinal, bool Cleaned)> visited = new();
            pending.Enqueue((start, false));
            while (pending.Count > 0)
            {
                var current = pending.Dequeue();
                if (!visited.Add(current)) continue;
                SemanticAsyncSegment segment = preview.Segments[current.Ordinal];
                bool cleaned = current.Cleaned || segment.Statements.Any(statement =>
                    statement.Operation.Span.Start == cleanupStart);
                if (cleaned && segment.Transfer?.Kind == terminalKind) return true;
                if (segment.Transfer?.PrimaryTarget >= 0)
                    pending.Enqueue((segment.Transfer.PrimaryTarget, cleaned));
                if (segment.Transfer?.SecondaryTarget >= 0)
                    pending.Enqueue((segment.Transfer.SecondaryTarget, cleaned));
            }
            return false;
        }
        Assert(ReachesAfterCleanup(dispatch.Transfer!.PrimaryTarget,
                SemanticAsyncMethod.ReturnTransferKind)
            && ReachesAfterCleanup(dispatch.Transfer.PrimaryTarget,
                SemanticAsyncMethod.PropagateFaultTransferKind)
            && ReachesAfterCleanup(dispatch.Transfer.SecondaryTarget,
                SemanticAsyncMethod.PropagateFaultTransferKind)
            && ReachesAfterCleanup(dispatch.Transfer.SecondaryTarget,
                SemanticAsyncMethod.ThrowTransferKind),
            "matched return, rethrow, unmatched fault, and replacement throw must traverse finally");
        byte[] bytes = SemanticSerializer.Serialize(document);
        Assert(bytes.SequenceEqual(SemanticSerializer.Serialize(
            SemanticSerializer.Deserialize(bytes))),
            "typed catch and cleanup preview must round-trip canonically");
        SemanticDocument replacingCatch = Analyze(source.Replace(
            "if (mode == 2) throw;",
            "if (mode == 2) throw new ArgumentException();",
            StringComparison.Ordinal), "Scripts/AsyncCatchReplacementPreview.cs");
        Assert(replacingCatch.RejectedAsyncExceptionFlows?.Single(item =>
                item.MethodSymbolId.Contains(".RunAsync(", StringComparison.Ordinal))
                .AsyncContinuationPreview is null,
            "a catch-local replacement throw must not publish a preview that skips finally");
    }

    private static void SuspendedFinallyPreviewRoutesTaskFailureThroughCleanup()
    {
        const string source = """
            using AvidScript;
            using System.Threading.Tasks;
            public static class Script
            {
                public static int CleanupCount;
                public static async Task<int> LoadAsync()
                {
                    await AvidContinuations.NextTickAsync();
                    return 12;
                }
                public static async Task<int> RunAsync()
                {
                    try
                    {
                        int value = await LoadAsync();
                        return value;
                    }
                    finally
                    {
                        CleanupCount++;
                    }
                }
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/AsyncFinallyFaultPreview.cs");
        SemanticAsyncExceptionPreview? preview = document.RejectedAsyncExceptionFlows
            ?.SingleOrDefault(flow => flow.MethodSymbolId.Contains(".RunAsync(",
                StringComparison.Ordinal))?.AsyncContinuationPreview;
        Assert(!document.Succeeded && preview is not null
            && document.Diagnostics.Any(item => item.Code == "ASCS5420")
            && document.ExceptionFlows is null
            && !SemanticAsyncInvocationValidator.IsValid(document),
            "a suspended finally preview must remain a rejected diagnostic artifact");
        SemanticAsyncSegment awaited = preview!.Segments.Single(segment =>
            segment.AwaitSite?.ProducerKind == "task_call");
        Assert(awaited.Transfer is { Kind: SemanticAsyncMethod.AwaitTransferKind,
                PrimaryTarget: >= 0, SecondaryTarget: >= 0 }
            && awaited.Transfer.PrimaryTarget != awaited.Transfer.SecondaryTarget,
            "a Task await inside try/finally needs separate success and failure successors");
        int cursor = awaited.Transfer!.SecondaryTarget;
        HashSet<int> visited = new();
        bool ranCleanup = false;
        bool propagated = false;
        while (visited.Add(cursor))
        {
            SemanticAsyncSegment segment = preview.Segments[cursor];
            ranCleanup |= segment.Statements.Any(statement =>
                statement.Operation.Span.Start == source.IndexOf("CleanupCount++",
                    StringComparison.Ordinal));
            if (segment.Transfer?.Kind == SemanticAsyncMethod.PropagateFaultTransferKind)
            {
                propagated = true;
                break;
            }
            if (segment.Transfer?.Kind != SemanticAsyncMethod.GotoTransferKind) break;
            cursor = segment.Transfer.PrimaryTarget;
        }
        Assert(ranCleanup && propagated,
            "a failed Task await must run finally once before propagating its in-flight fault");
        byte[] bytes = SemanticSerializer.Serialize(document);
        Assert(bytes.SequenceEqual(SemanticSerializer.Serialize(
            SemanticSerializer.Deserialize(bytes))),
            "the rejected continuation preview must round-trip canonically");
    }

    private static void RejectedAsyncExceptionRetainsRoslynRegions()
    {
        const string source = """
            using AvidScript;
            using System;
            using System.Threading.Tasks;
            public static class Script
            {
                public static async Task<int> LoadAsync()
                {
                    await AvidContinuations.NextTickAsync();
                    return 12;
                }
                public static async Task<int> RunAsync()
                {
                    try
                    {
                        return await LoadAsync();
                    }
                    catch (InvalidOperationException)
                    {
                        return 7;
                    }
                    finally
                    {
                        CleanupCount++;
                    }
                }
                public static int CleanupCount;
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/AsyncExceptionRegions.cs");
        SemanticExceptionFlow? flow = document.RejectedAsyncExceptionFlows?.SingleOrDefault(item =>
            item.MethodSymbolId.Contains(".RunAsync(", StringComparison.Ordinal));
        Assert(!document.Succeeded
            && document.Diagnostics.Any(item => item.Code == "ASCS5420")
            && document.Diagnostics.Any(item => item.Code == "ASCS3002")
            && flow is { Catches.Count: 1, Blocks.Count: > 0 }
            && flow.Catches[0].ExceptionTypeId
                == "type:global::System.InvalidOperationException"
            && flow.Regions.Any(region => region.Kind == "try_and_catch")
            && flow.Regions.Any(region => region.Kind == "finally")
            && document.AsyncMethods.All(method => method.MethodSymbolId != flow.MethodSymbolId)
            && document.ExceptionFlows is null
            && !SemanticAsyncInvocationValidator.IsValid(document)
            && !SemanticExceptionFlowContractValidator.IsValid(document),
            "a rejected async catch/finally must retain Roslyn regions for diagnostics without publishing executable flow: "
                + string.Join(" | ", document.Diagnostics.Select(item => item.Code + ":" + item.Message)));
        byte[] bytes = SemanticSerializer.Serialize(document);
        Assert(bytes.SequenceEqual(SemanticSerializer.Serialize(
            SemanticSerializer.Deserialize(bytes))),
            "the diagnostic async exception regions must round-trip canonically");
    }

    private static void AwaitFailureSuccessorKeepsCleanupLocalAlive()
    {
        SemanticSpan span = new(0, 1, 0, 0, 0, 1);
        const string localId = "symbol:local:cleanup_value";
        SemanticOperation Value(string kind, string? symbolId = null) => new(
            kind, true, null, false, false, false, false, "type:int32",
            symbolId, Array.Empty<string>(), null, null, null, null, null,
            span, Array.Empty<SemanticOperation>());
        SemanticAsyncSegment[] segments =
        {
            new(0, new[] { new SemanticAsyncStatement(Value("literal"), localId) },
                null, span, new(SemanticAsyncMethod.GotoTransferKind, null, 1)),
            new(1, Array.Empty<SemanticAsyncStatement>(),
                new SemanticAsyncAwaitSite(1, "task_call", "none",
                    Array.Empty<SemanticOperation>(), localId, "type:int32", span)
                { ResultStorageKind = "existing_local" },
                span, new(SemanticAsyncMethod.AwaitTransferKind, null, 2, 3)),
            new(2, Array.Empty<SemanticAsyncStatement>(), null, span,
                new(SemanticAsyncMethod.ReturnTransferKind, Value("literal"), -1)),
            new(3, Array.Empty<SemanticAsyncStatement>(), null, span,
                new(SemanticAsyncMethod.ReturnTransferKind,
                    Value("local_reference", localId), -1)),
        };
        SemanticAsyncStateFlowAnalysis analysis =
            SemanticAsyncStateFlowAnalyzer.AnalyzeControlFlow(segments);
        Assert(analysis.Issues.Count == 0
            && analysis.SlotsByAwaitSegment.TryGetValue(1, out var saved)
            && saved.Count == 1 && saved[0].SymbolId == localId,
            "the old value of an await assignment target must survive for a failure cleanup");
        Assert(SemanticAsyncScopeValidator.GetEntries(segments, 0, new[] { 3 })
                .SequenceEqual(new[] { new SemanticClosureEntry(1, 3) }),
            "an await failure edge entering a lexical scope must allocate that scope");
        Assert(SemanticAsyncScopeValidator.Targets(
                new(SemanticAsyncMethod.AwaitTransferKind, null, 2))
                .SequenceEqual(new[] { 2 }),
            "older await transfers still have only a success edge");

        SemanticAsyncSegment[] invalid = (SemanticAsyncSegment[])segments.Clone();
        invalid[1] = segments[1] with
        {
            Transfer = segments[1].Transfer! with { SecondaryTarget = 99 },
        };
        Assert(SemanticAsyncStateFlowAnalyzer.AnalyzeControlFlow(invalid).Issues
                .Any(issue => issue.Message.Contains("missing segment 99", StringComparison.Ordinal)),
            "a missing await failure successor must fail closed");
    }

    private static void TaskIntThrowPublishesVersionedErrorPlan()
    {
        const string source = """
            using AvidScript;
            using System;
            using System.Runtime.InteropServices;
            using System.Threading.Tasks;
            public static class Script
            {
                public static async Task<int> LoadAsync(bool fail)
                {
                    await AvidContinuations.NextTickAsync();
                    if (fail) throw new InvalidOperationException();
                    return 12;
                }
                public static async void BeginPlay()
                {
                    await AvidContinuations.NextTickAsync();
                }
                [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
                public static void Tick(float deltaSeconds)
                {
                    float elapsed = deltaSeconds + 1.0f;
                }
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/TaskIntThrow.cs");
        Assert(!document.Succeeded
            && document.Diagnostics.Any(item => item.Code == "ASCS5422"
                && item.Span.Start == source.IndexOf("throw new", StringComparison.Ordinal))
            && document.SchemaVersion == SemanticContract.AsyncLanguageErrorSchemaVersion
            && document.SemanticVersion == SemanticContract.AsyncLanguageErrorSemanticVersion
            && document.AsyncMethods.Count == 2
            && document.ControlFlowGraphs.Any(graph =>
                graph.MethodSymbolId.Contains(".Tick(", StringComparison.Ordinal))
            && SemanticAsyncInvocationValidator.IsValid(document)
            && SemanticAsyncErrorPlanValidator.IsValid(document),
            "async Task<int> throw must publish a validated error plan: "
                + string.Join(" | ", document.Diagnostics.Select(item => item.Code + ":" + item.Message)));
        SemanticAsyncMethod method = document.AsyncMethods.Single(item => item.ErrorPlan is not null);
        SemanticAsyncThrowSite site = method.ErrorPlan!.Throws.Single();
        SemanticAsyncSegment segment = method.Segments.Single(item =>
            item.Transfer?.Kind == SemanticAsyncMethod.ThrowTransferKind);
        Assert(method.ErrorPlan.SourceId == document.Source.SourceId
            && method.ErrorPlan.SourceLength == document.Source.Length
            && site.SegmentOrdinal == segment.Ordinal
            && site.Span == segment.Span
            && site.ExceptionTypeId == "type:global::System.InvalidOperationException"
            && segment.Transfer!.Condition is
                { Kind: "object_creation", Children.Count: 0 },
            "the throw transfer must retain source, constructor and segment identity");
        byte[] bytes = SemanticSerializer.Serialize(document);
        Assert(bytes.SequenceEqual(SemanticSerializer.Serialize(
            SemanticSerializer.Deserialize(bytes))),
            "the async error plan must round-trip canonically");
        Assert(!SemanticAsyncInvocationValidator.IsValid(document with
        {
            SchemaVersion = SemanticContract.TaskLanguageErrorSchemaVersion,
            SemanticVersion = SemanticContract.TaskLanguageErrorSemanticVersion,
        }), "schema 40 must reject async language-error metadata");
        Assert(!SemanticAsyncInvocationValidator.IsValid(document with
        {
            AsyncMethods = document.AsyncMethods.Select(candidate => candidate == method
                ? candidate with
                {
                    Segments = candidate.Segments.Select(item => item.Ordinal == segment.Ordinal
                        ? item with
                        {
                            Transfer = item.Transfer! with
                            {
                                ExceptionTypeId = site.ExceptionTypeId,
                            },
                        }
                        : item).ToArray(),
                }
                : candidate).ToArray(),
        }), "the current executable async schema must reject preview-only catch metadata");
        Assert(!SemanticAsyncInvocationValidator.IsValid(document with
        {
            AsyncMethods = new[] { method with
            {
                ErrorPlan = method.ErrorPlan with { Throws = new[]
                {
                    site with { SegmentOrdinal = method.EntrySegmentOrdinal }
                } }
            } }
        }), "a forged throw segment must fail validation");
        Assert(!SemanticAsyncInvocationValidator.IsValid(document with
        {
            AsyncMethods = new[] { method with
            {
                ErrorPlan = method.ErrorPlan with { Throws = new[]
                {
                    site with { ConstructorSymbolId = "symbol:forged" }
                } }
            } }
        }), "a forged exception constructor must fail validation");
        SemanticDocument unsupported = Analyze(source.Replace(
            "new InvalidOperationException()",
            "new InvalidOperationException(\"message\")", StringComparison.Ordinal),
            "Scripts/TaskIntThrowArguments.cs");
        Assert(!unsupported.Succeeded && unsupported.ControlFlowGraphs.Count == 0
            && unsupported.Diagnostics.Any(item => item.Code == "ASCS5421"),
            "a parameterized exception constructor must fail at the source location: graphs="
                + unsupported.ControlFlowGraphs.Count + " diagnostics="
                + string.Join(" | ", unsupported.Diagnostics.Select(item => item.Code)));
        SemanticDocument cleanup = Analyze(source.Replace(
            "if (fail) throw new InvalidOperationException();",
            "try { if (fail) throw new InvalidOperationException(); } finally { }",
            StringComparison.Ordinal), "Scripts/TaskIntThrowCleanup.cs");
        Assert(!cleanup.Succeeded && cleanup.Diagnostics.Any(item => item.Code == "ASCS5420"),
            "a throw inside try/finally must not skip the cleanup edge");
    }

    private static void TaskIntAliasesPreserveEveryOwner()
    {
        const string source = """
            using AvidScript;
            using System.Threading.Tasks;
            public static class Script
            {
                public static int Result;
                public static async Task<int> LoadScoreAsync()
                {
                    await AvidContinuations.NextTickAsync();
                    return 12;
                }
                public static async void BeginPlay()
                {
                    Task<int> original = LoadScoreAsync();
                    Task<int> alias = original;
                    Task<int> last = alias;
                    await AvidContinuations.NextTickAsync();
                    Result = await last;
                }
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/TaskIntAliases.cs");
        Assert(document.Succeeded
            && document.SchemaVersion == SemanticContract.TaskAliasSchemaVersion
            && document.SemanticVersion == SemanticContract.TaskAliasSemanticVersion
            && SemanticAsyncInvocationValidator.IsValid(document),
            "Task aliases must select Semantic 39 with validated provenance: "
                + string.Join(" | ", document.Diagnostics.Select(item => item.Code + ":" + item.Message)));
        SemanticAsyncMethod consumer = document.AsyncMethods.Single(method => method.TaskResultTypeId is null);
        SemanticAsyncAwaitSite site = consumer.Segments.Select(segment => segment.AwaitSite)
            .Single(awaitSite => awaitSite?.ProducerKind == "task_local")!;
        Assert(consumer.TaskLocalSymbolIds is { Count: 3 }
            && site.TaskLocalSymbolId == consumer.TaskLocalSymbolIds[2]
            && consumer.Segments.Any(segment => segment.AwaitSite?.StateFrame?.Slots
                .Any(slot => slot.SymbolId == site.TaskLocalSymbolId) == true),
            "all aliases retain a separate owner even if only the last alias is awaited");
        byte[] bytes = SemanticSerializer.Serialize(document);
        Assert(bytes.SequenceEqual(SemanticSerializer.Serialize(SemanticSerializer.Deserialize(bytes))),
            "Task alias ownership metadata must round-trip canonically");
        Assert(!SemanticAsyncInvocationValidator.IsValid(document with
        {
            SchemaVersion = SemanticContract.TaskExistingLocalSchemaVersion,
            SemanticVersion = SemanticContract.TaskExistingLocalSemanticVersion,
        }), "older semantic labels cannot acquire alias ownership");
        Assert(!SemanticAsyncInvocationValidator.IsValid(document with
        {
            AsyncMethods = document.AsyncMethods.Select(method => method == consumer
                ? method with { TaskLocalSymbolIds = method.TaskLocalSymbolIds!.Skip(1).ToArray() }
                : method).ToArray(),
        }), "omitting an unawaited source owner must be rejected");
        Assert(!SemanticAsyncInvocationValidator.IsValid(document with
        {
            AsyncMethods = document.AsyncMethods.Select(method => method == consumer
                ? method with { TaskLocalSymbolIds = Array.Empty<string>() }
                : method).ToArray(),
        }), "an empty alias ownership list cannot claim Semantic 39");
        Assert(!SemanticAsyncInvocationValidator.IsValid(document with
        {
            AsyncMethods = document.AsyncMethods.Select(method => method == consumer
                ? method with { TaskLocalSymbolIds = method.TaskLocalSymbolIds!.Reverse().ToArray() }
                : method).ToArray(),
        }), "Task alias owner order must match source declarations");
        SemanticDocument interleaved = Analyze(source.Replace(
                "Task<int> original = LoadScoreAsync();",
                "Result = 1; Task<int> original = LoadScoreAsync();", StringComparison.Ordinal)
            .Replace("Task<int> alias = original;",
                "Task<int> alias = original; Result = 2;", StringComparison.Ordinal),
            "Scripts/TaskIntInterleavedAliases.cs");
        Assert(interleaved.Succeeded && SemanticAsyncInvocationValidator.IsValid(interleaved),
            "straight-line synchronous statements may precede and separate task owners: "
                + string.Join(" | ", interleaved.Diagnostics.Select(item => item.Message)));
        SemanticDocument branchingOwner = Analyze(source.Replace(
            "Task<int> alias = original;",
            "if (Result == 0) return; Task<int> alias = original;", StringComparison.Ordinal),
            "Scripts/TaskIntBranchedOwner.cs");
        Assert(branchingOwner.Succeeded
            && SemanticAsyncInvocationValidator.IsValid(branchingOwner),
            "an early return between owners may release only the task already created: "
                + string.Join(" | ", branchingOwner.Diagnostics.Select(item => item.Message)));
        SemanticDocument reassignment = Analyze(source.Replace("Task<int> last = alias;",
            "Task<int> last = alias; last = original;", StringComparison.Ordinal),
            "Scripts/TaskIntReassignedAlias.cs");
        Assert(!reassignment.Succeeded && reassignment.Diagnostics.Any(item => item.Code == "ASCS5403"),
            "reassignment remains rejected until path-dependent ownership is represented");
    }

    private static void TaskIntExistingLocalAssignmentPreservesStorage()
    {
        const string source = """
            using AvidScript;
            using System.Threading.Tasks;
            public static class Script
            {
                public static int Result;
                public static async Task<int> LoadScoreAsync()
                {
                    await AvidContinuations.NextTickAsync();
                    return 12;
                }
                public static async void BeginPlay()
                {
                    int score = 0;
                    score = await LoadScoreAsync();
                    Result = score;
                }
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/TaskIntExistingLocal.cs");
        Assert(document.Succeeded
            && document.SchemaVersion == SemanticContract.TaskExistingLocalSchemaVersion
            && document.SemanticVersion == SemanticContract.TaskExistingLocalSemanticVersion
            && SemanticAsyncInvocationValidator.IsValid(document),
            "await assignment to an existing int local needs Semantic 38");
        SemanticAsyncMethod consumer = document.AsyncMethods.Single(method =>
            method.TaskResultTypeId is null);
        SemanticAsyncAwaitSite site = consumer.Segments.Select(segment => segment.AwaitSite)
            .Single(awaitSite => awaitSite?.ProducerKind == "task_call")!;
        Assert(site.ResultStorageKind == "existing_local"
            && document.Symbols.Any(symbol => symbol.Id == site.ResultSymbolId
                && symbol.Kind == "local" && symbol.Name == "score")
            && consumer.Segments.All(segment => segment.AwaitSite?.StateFrame?.Slots
                .Any(slot => slot.SymbolId == site.ResultSymbolId) != true),
            "await overwrites the local on resume instead of saving its previous value");
        byte[] bytes = SemanticSerializer.Serialize(document);
        Assert(bytes.SequenceEqual(SemanticSerializer.Serialize(SemanticSerializer.Deserialize(bytes))),
            "existing-local assignment metadata must round-trip canonically");
        Assert(!SemanticAsyncInvocationValidator.IsValid(document with
        {
            SchemaVersion = SemanticContract.TaskAssignmentSchemaVersion,
            SemanticVersion = SemanticContract.TaskAssignmentSemanticVersion,
        }), "Semantic 37 cannot acquire existing-local assignment by relabeling");
        Assert(!SemanticAsyncInvocationValidator.IsValid(document with
        {
            AsyncMethods = document.AsyncMethods.Select(method => method == consumer
                ? method with { Segments = method.Segments.Select(segment => segment.AwaitSite == site
                    ? segment with { AwaitSite = site with { ResultSymbolId = "symbol:forged" } }
                    : segment).ToArray() }
                : method).ToArray(),
        }), "a forged existing-local result must be rejected");
        SemanticDocument uninitialized = Analyze(source.Replace("int score = 0;",
            "int score;", StringComparison.Ordinal), "Scripts/TaskIntUninitializedLocal.cs");
        Assert(uninitialized.Succeeded && SemanticAsyncInvocationValidator.IsValid(uninitialized),
            "a declared local without an initializer can receive an awaited result");
    }

    private static void TaskIntStaticFieldAssignmentIsVersionedAndBounded()
    {
        const string source = """
            using AvidScript;
            using System.Threading.Tasks;
            public static class Script
            {
                public static int Result;
                public static async Task<int> LoadScoreAsync()
                {
                    await AvidContinuations.NextTickAsync();
                    return 12;
                }
                public static async void BeginPlay()
                {
                    Result = await LoadScoreAsync();
                }
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/TaskIntStaticAssignment.cs");
        Assert(document.Succeeded
            && document.SchemaVersion == SemanticContract.TaskAssignmentSchemaVersion
            && document.SemanticVersion == SemanticContract.TaskAssignmentSemanticVersion
            && SemanticAsyncInvocationValidator.IsValid(document),
            "direct Task<int> field assignment must select a validated Semantic contract");
        SemanticAsyncMethod consumer = document.AsyncMethods.Single(method =>
            method.TaskResultTypeId is null);
        SemanticAsyncAwaitSite site = consumer.Segments.Select(segment => segment.AwaitSite)
            .Single(awaitSite => awaitSite?.ProducerKind == "task_call")!;
        Assert(site.ResultStorageKind == "static_field"
            && document.Symbols.Any(symbol => symbol.Id == site.ResultSymbolId
                && symbol.Kind == "field" && symbol.Name == "Result")
            && consumer.Segments.All(segment => segment.AwaitSite?.StateFrame?.Slots
                .Any(slot => slot.SymbolId == site.ResultSymbolId) != true),
            "static result is a global write, not a continuation frame local");
        byte[] bytes = SemanticSerializer.Serialize(document);
        Assert(bytes.SequenceEqual(SemanticSerializer.Serialize(SemanticSerializer.Deserialize(bytes))),
            "field assignment metadata must round-trip canonically");
        Assert(!SemanticAsyncInvocationValidator.IsValid(document with
        {
            SchemaVersion = SemanticContract.TaskLocalSchemaVersion,
            SemanticVersion = SemanticContract.TaskLocalSemanticVersion,
        }), "older Semantic versions cannot acquire field assignment by relabeling");
        Assert(!SemanticAsyncInvocationValidator.IsValid(document with
        {
            AsyncMethods = document.AsyncMethods.Select(method => method == consumer
                ? method with { Segments = method.Segments.Select(segment => segment.AwaitSite == site
                    ? segment with { AwaitSite = site with { ResultSymbolId = "symbol:forged" } }
                    : segment).ToArray() }
                : method).ToArray(),
        }), "a forged result field must be rejected");
        SemanticDocument property = Analyze(source.Replace("public static int Result;",
            "public static int Result { get; set; }", StringComparison.Ordinal),
            "Scripts/TaskIntPropertyAssignment.cs");
        Assert(!property.Succeeded && property.Diagnostics.Any(item => item.Code == "ASCS5404"),
            "property targets stay rejected until pre-await receiver evaluation is modeled");
    }

    private static void TaskIntParallelLocalsPreserveBothOwners()
    {
        const string source = """
            using AvidScript;
            using System.Threading.Tasks;
            public static class Script
            {
                public static async Task<int> LoadScoreAsync(int score)
                {
                    await AvidContinuations.NextTickAsync();
                    return score;
                }
                public static async void BeginPlay()
                {
                    Task<int> left = LoadScoreAsync(7);
                    Task<int> right = LoadScoreAsync(5);
                    await AvidContinuations.NextTickAsync();
                    int first = await left;
                    int second = await right;
                }
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/TaskIntParallelLocals.cs");
        Assert(document.Succeeded && SemanticAsyncInvocationValidator.IsValid(document),
            "two leading Task<int> locals must have validated producer provenance: "
                + string.Join(" | ", document.Diagnostics.Select(item => item.Code + ":" + item.Message)));
        SemanticAsyncMethod consumer = document.AsyncMethods.Single(method => method.TaskResultTypeId is null);
        string[] locals = consumer.Segments.Select(segment => segment.AwaitSite?.TaskLocalSymbolId)
            .OfType<string>().Distinct(StringComparer.Ordinal).ToArray();
        SemanticAsyncAwaitSite firstAwait = consumer.Segments.Select(segment => segment.AwaitSite)
            .First(site => site is not null)!;
        Assert(locals.Length == 2 && locals.All(id => firstAwait.StateFrame?.Slots
                .Any(slot => slot.SymbolId == id) == true),
            "the first suspension frame must preserve both live task locals");
        SemanticDocument unawaited = Analyze(source.Replace(
            "Task<int> right = LoadScoreAsync(5);",
            "Task<int> right = LoadScoreAsync(5); Task<int> ignored = LoadScoreAsync(8);",
            StringComparison.Ordinal), "Scripts/TaskIntUnawaitedLocal.cs");
        Assert(!unawaited.Succeeded && unawaited.Diagnostics.Any(item => item.Code == "ASCS5403"),
            "an unawaited Task local must not leak its producer reference");
        SemanticDocument noTaskAwait = Analyze(source.Replace(
            "int first = await left;", "int first = 0;", StringComparison.Ordinal)
            .Replace("int second = await right;", "int second = 0;", StringComparison.Ordinal),
            "Scripts/TaskIntNoAwait.cs");
        Assert(!noTaskAwait.Succeeded,
            "Task locals without any await must not escape ownership tracking");
        string sixDeclarations = string.Join(" ", Enumerable.Range(0, 6)
            .Select(index => $"Task<int> extra{index} = LoadScoreAsync({index});"));
        string sixAwaits = string.Join(" ", Enumerable.Range(0, 6)
            .Select(index => $"await extra{index};"));
        SemanticDocument atBudget = Analyze(source.Replace(
                "Task<int> right = LoadScoreAsync(5);",
                "Task<int> right = LoadScoreAsync(5); " + sixDeclarations,
                StringComparison.Ordinal).Replace("int second = await right;",
                "int second = await right; " + sixAwaits, StringComparison.Ordinal),
            "Scripts/TaskIntEightLocals.cs");
        Assert(atBudget.Succeeded && SemanticAsyncInvocationValidator.IsValid(atBudget),
            "eight leading Task locals must remain inside the ownership budget");
        string extraDeclarations = string.Join(" ", Enumerable.Range(0, 7)
            .Select(index => $"Task<int> extra{index} = LoadScoreAsync({index});"));
        string extraAwaits = string.Join(" ", Enumerable.Range(0, 7)
            .Select(index => $"await extra{index};"));
        SemanticDocument overBudget = Analyze(source.Replace(
                "Task<int> right = LoadScoreAsync(5);",
                "Task<int> right = LoadScoreAsync(5); " + extraDeclarations,
                StringComparison.Ordinal).Replace("int second = await right;",
                "int second = await right; " + extraAwaits, StringComparison.Ordinal),
            "Scripts/TaskIntTooManyLocals.cs");
        Assert(!overBudget.Succeeded
            && overBudget.Diagnostics.Any(item => item.Code == "ASCS5403"),
            "the bounded Task local ownership profile must reject a ninth local");
    }

    private static void TaskIntLocalPublishesProducerAndFrame()
    {
        const string source = """
            using AvidScript;
            using System.Threading.Tasks;
            public static class Script
            {
                public static async Task<int> LoadScoreAsync()
                {
                    await AvidContinuations.NextTickAsync();
                    return 12;
                }
                public static async void BeginPlay()
                {
                    Task<int> pending = LoadScoreAsync();
                    await AvidContinuations.NextTickAsync();
                    int score = await pending;
                }
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/TaskIntLocal.cs");
        Assert(document.Succeeded, "Task<int> local must analyze: "
            + string.Join(" | ", document.Diagnostics.Select(item => item.Message)));
        Assert(document.SchemaVersion == SemanticContract.TaskLocalSchemaVersion
            && document.SemanticVersion == SemanticContract.TaskLocalSemanticVersion,
            "Task local selects its own semantic contract");
        SemanticAsyncMethod consumer = document.AsyncMethods.Single(method => method.TaskResultTypeId is null);
        SemanticAsyncAwaitSite site = consumer.Segments.Select(segment => segment.AwaitSite)
            .Single(awaitSite => awaitSite?.ProducerKind == "task_local")!;
        Assert(site.TaskLocalSymbolId is not null && site.TaskCallableId is not null
            && site.Arguments.Single().SymbolId == site.TaskLocalSymbolId
            && consumer.Segments.Any(segment => segment.AwaitSite?.StateFrame?.Slots
                .Any(slot => slot.SymbolId == site.TaskLocalSymbolId) == true)
            && SemanticAsyncInvocationValidator.IsValid(document),
            "Task local publishes exact producer provenance and survives the preceding await");
        byte[] bytes = SemanticSerializer.Serialize(document);
        Assert(bytes.SequenceEqual(SemanticSerializer.Serialize(SemanticSerializer.Deserialize(bytes))),
            "Task local semantic metadata round-trips canonically");
        Assert(!SemanticAsyncInvocationValidator.IsValid(document with
        {
            AsyncMethods = document.AsyncMethods.Select(method => method == consumer
                ? method with { Segments = method.Segments.Select(segment => segment.AwaitSite == site
                    ? segment with { AwaitSite = site with { TaskCallableId = "symbol:forged" } }
                    : segment).ToArray() }
                : method).ToArray(),
        }), "forged Task local producer is rejected");
        Assert(!SemanticAsyncInvocationValidator.IsValid(document with
        {
            AsyncMethods = document.AsyncMethods.Select(method => method == consumer
                ? method with { Segments = method.Segments.Select(segment =>
                    segment.AwaitSite is { ProducerKind: "next_tick", StateFrame: { } frame }
                        ? segment with { AwaitSite = segment.AwaitSite with
                            { StateFrame = frame with { Slots = frame.Slots.Where(slot =>
                                slot.SymbolId != site.TaskLocalSymbolId).ToArray() } } }
                        : segment).ToArray() }
                : method).ToArray(),
        }), "Task local cannot disappear from a pending continuation frame");
        SemanticDocument alias = Analyze(source.Replace("int score = await pending;",
            "Task<int> copied = pending; int score = await pending;", StringComparison.Ordinal),
            "Scripts/TaskIntAlias.cs");
        Assert(!alias.Succeeded && alias.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS5403"),
            "Task aliasing stays rejected until ownership transfer is represented");
        SemanticDocument late = Analyze(source.Replace("Task<int> pending = LoadScoreAsync();",
            "await AvidContinuations.NextTickAsync(); Task<int> pending = LoadScoreAsync();",
            StringComparison.Ordinal), "Scripts/TaskIntLateLocal.cs");
        Assert(late.Succeeded && SemanticAsyncInvocationValidator.IsValid(late),
            "Task creation after an earlier await must have validated path ownership: "
                + string.Join(" | ", late.Diagnostics.Select(item => item.Message)));
        SemanticAsyncMethod lateConsumer = late.AsyncMethods.Single(method => method.TaskResultTypeId is null);
        string lateTask = lateConsumer.Segments.Select(segment => segment.AwaitSite?.TaskLocalSymbolId)
            .OfType<string>().Single();
        Assert(SemanticAsyncInvocationValidator.TryGetTaskLocalFlow(
                lateConsumer, new[] { lateTask }, false, out _, out _,
                out IReadOnlyDictionary<int, IReadOnlyList<string>> before,
                out IReadOnlyDictionary<int, IReadOnlyList<string>> after),
            "late task ownership must be derivable from the CFG");
        SemanticAsyncSegment[] ticks = lateConsumer.Segments.Where(segment =>
            segment.AwaitSite?.ProducerKind == "next_tick").OrderBy(segment =>
            segment.Ordinal).ToArray();
        Assert(ticks.Length == 2 && after[ticks[0].Ordinal].Count == 0
            && after[ticks[1].Ordinal].SequenceEqual(new[] { lateTask })
            && lateConsumer.Segments.Any(segment => before.TryGetValue(segment.Ordinal,
                out IReadOnlyList<string>? active) && active.Contains(lateTask)),
            "only the second suspension may transfer the late task owner");
        const string conditionalSource = """
            using AvidScript;
            using System.Threading.Tasks;
            public static class Script
            {
                public static int Flag;
                public static async Task<int> LoadScoreAsync()
                {
                    await AvidContinuations.NextTickAsync();
                    return 12;
                }
                public static async void BeginPlay()
                {
                    if (Flag == 0)
                    {
                        await AvidContinuations.NextTickAsync();
                        Task<int> pending = LoadScoreAsync();
                        int score = await pending;
                        return;
                    }
                    return;
                }
            }
            """;
        SemanticDocument conditional = Analyze(conditionalSource, "Scripts/TaskIntConditionalLocal.cs");
        Assert(conditional.Succeeded && SemanticAsyncInvocationValidator.IsValid(conditional),
            "a conditional early return may skip a later Task local declaration: "
                + string.Join(" | ", conditional.Diagnostics.Select(item => item.Message)));
        const string mergingSource = """
            using AvidScript;
            using System.Threading.Tasks;
            public static class Script
            {
                public static int Flag;
                public static async Task<int> LoadScoreAsync()
                {
                    await AvidContinuations.NextTickAsync();
                    return 12;
                }
                public static async void BeginPlay()
                {
                    if (Flag == 0)
                    {
                        Task<int> first = LoadScoreAsync();
                        int left = await first;
                    }
                    else
                    {
                        Task<int> second = LoadScoreAsync();
                        int right = await second;
                    }
                }
            }
            """;
        SemanticDocument merging = Analyze(mergingSource, "Scripts/TaskIntMergingOwners.cs");
        Assert(!merging.Succeeded && merging.Diagnostics.Any(item => item.Code == "ASCS5403"),
            "branches with different live Task owners must be rejected at their join");
    }

    private static void TaskIntSuspendedCleanupFailsClosed()
    {
        const string source = """
            using AvidScript;
            using System.Threading.Tasks;
            public static class Script
            {
                private static int Cleanups;
                public static async Task<int> LoadScoreAsync()
                {
                    try
                    {
                        await AvidContinuations.NextTickAsync();
                        return 12;
                    }
                    finally
                    {
                        Cleanups++;
                    }
                }
            }
            """;
        SemanticDocument rejected = Analyze(source, "Scripts/TaskIntSuspendedCleanup.cs");
        Assert(!rejected.Succeeded && rejected.Diagnostics.Any(item => item.Code == "ASCS5420")
            && rejected.RejectedAsyncExceptionFlows is { Count: 1 } flows
            && flows[0].Regions.Any(region => region.Kind == "finally")
            && rejected.ExceptionFlows is null,
            "await inside try/finally must fail until suspended cleanup is owned by the task");
    }

    private static void TaskIntAwaitProjectsValueArguments()
    {
        const string source = """
            using AvidScript;
            using System.Threading.Tasks;
            public static class Script
            {
                public static async Task<int> AddAsync(int left, int right)
                {
                    await AvidContinuations.NextTickAsync();
                    return left + right;
                }
                public static async void BeginPlay()
                {
                    int result = await AddAsync(7, 5);
                }
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/TaskIntArguments.cs");
        Assert(document.Succeeded, "Task<int> value arguments must analyze successfully: "
            + string.Join(" | ", document.Diagnostics.Select(item => item.Message)));
        SemanticAsyncMethod consumer = document.AsyncMethods.Single(method => method.TaskResultTypeId is null);
        SemanticAsyncAwaitSite site = consumer.Segments.Select(segment => segment.AwaitSite)
            .Single(awaitSite => awaitSite?.ProducerKind == "task_call")!;
        Assert(site.Arguments.Count == 2
            && site.Arguments.All(argument => argument.TypeId == "type:int32")
            && SemanticAsyncInvocationValidator.IsValid(document)
            && SemanticClosureContractValidator.IsValid(document),
            "Task<int> call preserves ordered, typed arguments across the semantic boundary");
        byte[] bytes = SemanticSerializer.Serialize(document);
        Assert(bytes.SequenceEqual(SemanticSerializer.Serialize(SemanticSerializer.Deserialize(bytes))),
            "parameterized Task<int> metadata round-trips canonically");
        Assert(!SemanticAsyncInvocationValidator.IsValid(document with
        {
            AsyncMethods = document.AsyncMethods.Select(method => method == consumer
                ? method with { Segments = method.Segments.Select(segment => segment.AwaitSite == site
                    ? segment with { AwaitSite = site with { Arguments = site.Arguments.Take(1).ToArray() } }
                    : segment).ToArray() }
                : method).ToArray()
        }), "Task<int> argument count tampering is rejected");
        Assert(!SemanticAsyncInvocationValidator.IsValid(document with
        {
            AsyncMethods = document.AsyncMethods.Select(method => method == consumer
                ? method with { Segments = method.Segments.Select(segment => segment.AwaitSite == site
                    ? segment with { AwaitSite = site with { Arguments = new[]
                        { site.Arguments[0] with { TypeId = "type:float32" }, site.Arguments[1] } } }
                    : segment).ToArray() }
                : method).ToArray()
        }), "Task<int> argument type tampering is rejected");
        SemanticDocument reordered = Analyze(source.Replace("AddAsync(7, 5)",
            "AddAsync(right: 5, left: 7)", StringComparison.Ordinal),
            "Scripts/TaskIntReorderedArguments.cs");
        Assert(!reordered.Succeeded && reordered.Diagnostics.Any(item => item.Code == "ASCS5403"),
            "reordered named arguments are rejected until evaluation order is represented");
    }

    private static void TaskAndExceptionPlansKeepBothContracts()
    {
        const string source = """
            using AvidScript;
            using System;
            using System.Threading.Tasks;
            public static class Script
            {
                public static async Task<int> LoadScoreAsync()
                {
                    await AvidContinuations.NextTickAsync();
                    return 12;
                }
                private static int Handle()
                {
                    try { throw new InvalidOperationException(); }
                    catch (InvalidOperationException) { return 1; }
                }
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/TaskAndExceptionPlan.cs");
        Assert(!document.Succeeded
            && document.SchemaVersion == SemanticContract.TaskLanguageErrorSchemaVersion
            && document.SemanticVersion == SemanticContract.TaskLanguageErrorSemanticVersion
            && document.AsyncMethods.Count == 1
            && document.ExceptionFlows?.Count == 1
            && SemanticExceptionFlowContractValidator.IsValid(document)
            && SemanticAsyncInvocationValidator.IsValid(document),
            "a source with disjoint Task and exception methods retains both validated plans");
        byte[] serialized = SemanticSerializer.Serialize(document);
        Assert(serialized.SequenceEqual(SemanticSerializer.Serialize(
            SemanticSerializer.Deserialize(serialized))),
            "the combined semantic contract round-trips canonically");
        Assert(!SemanticExceptionFlowContractValidator.IsValid(document with
        {
            ExceptionFlows = null,
        }), "Semantic 40 requires its exception plan");
        Assert(!SemanticExceptionFlowContractValidator.IsValid(document with
        {
            AsyncMethods = Array.Empty<SemanticAsyncMethod>(),
        }), "Semantic 40 requires its Task plan");
        Assert(!SemanticExceptionFlowContractValidator.IsValid(document with
        {
            SchemaVersion = SemanticContract.ExceptionFlowSchemaVersion,
            SemanticVersion = SemanticContract.ExceptionFlowSemanticVersion,
        }), "Semantic 34 cannot erase the Task/error version boundary");
        Assert(!SemanticExceptionFlowContractValidator.IsValid(document with
        {
            AsyncMethods = document.AsyncMethods.Select(method => method with
            {
                MethodSymbolId = document.ExceptionFlows![0].MethodSymbolId,
            }).ToArray(),
        }), "one method cannot publish incompatible Task and exception CFGs");
    }

    private static void TaskIntAwaitPublishesDirectTarget()
    {
        const string source = """
            using AvidScript;
            using System.Threading.Tasks;
            public static class Script
            {
                public static async Task<int> LoadScoreAsync()
                {
                    await AvidContinuations.NextTickAsync();
                    return 12;
                }
                public static async void BeginPlay()
                {
                    int score = await LoadScoreAsync();
                }
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/TaskIntAwait.cs");
        Assert(document.Succeeded, "direct Task<int> await source must analyze successfully");
        SemanticAsyncMethod producer = document.AsyncMethods.Single(method => method.TaskResultTypeId is not null);
        SemanticAsyncMethod consumer = document.AsyncMethods.Single(method => method.TaskResultTypeId is null);
        Assert(consumer.Lowering == SemanticAsyncMethod.ContinuationCfgLowering,
            "direct Task<int> await must use the CFG needed for immediate and deferred completion");
        SemanticAsyncAwaitSite site = consumer
            .Segments.Select(segment => segment.AwaitSite)
            .Single(awaitSite => awaitSite?.ProducerKind == "task_call")!;
        Assert(site.TaskCallableId == producer.MethodSymbolId
            && site.ResultTypeId == "type:int32"
            && site.PayloadKind == "task_result"
            && site.ResultSymbolId is not null,
            "Task<int> await publishes its exact source target and result local");
        Assert(SemanticClosureContractValidator.IsValid(document),
            "the direct task await passes semantic readers");
        byte[] serialized = SemanticSerializer.Serialize(document);
        Assert(serialized.SequenceEqual(SemanticSerializer.Serialize(SemanticSerializer.Deserialize(serialized)))
            && serialized.SequenceEqual(SemanticSerializer.Serialize(Analyze(source, "Scripts/TaskIntAwait.cs"))),
            "Task<int> invocation and result serialize canonically and deterministically");
        Assert(!SemanticAsyncInvocationValidator.IsValid(document with
        {
            AsyncMethods = document.AsyncMethods.Select(method => method.TaskResultTypeId is null
                ? method with { Segments = method.Segments.Select(segment => segment.AwaitSite == site
                    ? segment with { AwaitSite = site with { TaskCallableId = "symbol:unknown" } }
                    : segment).ToArray() }
                : method).ToArray()
        }), "task await target tampering is rejected");

        SemanticDocument unsupported = Analyze(source.Replace("Task<int>", "Task<double>")
            .Replace("return 12;", "return 12.0;"), "Scripts/UnsupportedTaskResult.cs");
        Assert(!unsupported.Succeeded && unsupported.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS5401"),
            "unsupported Task<double> remains rejected at its declaration");
    }

    private static void TaskIntResultProjectsTypedReturn()
    {
        const string source = """
            using AvidScript;
            using System.Threading.Tasks;
            public static class Script
            {
                public static async Task<int> LoadScoreAsync()
                {
                    await AvidContinuations.NextTickAsync();
                    return 12;
                }
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/TaskIntResult.cs");
        Assert(document.Succeeded, "Task<int> producer source must analyze successfully");
        Assert(document.SchemaVersion == SemanticContract.TaskResultSchemaVersion
            && document.SemanticVersion == SemanticContract.TaskResultSemanticVersion,
            "Task<int> producer selects its versioned semantic contract");
        SemanticAsyncMethod method = document.AsyncMethods.Single();
        Assert(method.TaskResultTypeId == "type:int32"
            && method.ExportName is null
            && method.Lowering == SemanticAsyncMethod.ContinuationCfgLowering,
            "Task<int> producer publishes the underlying result type and CFG lowering");
        Assert(method.Segments.Any(segment => segment.Transfer is
            { Kind: SemanticAsyncMethod.ReturnTransferKind, Condition.TypeId: "type:int32" }),
            "Task<int> return carries the converted int result");
        Assert(SemanticAsyncInvocationValidator.IsValid(document),
            "Task<int> producer passes the independent invocation contract");
        Assert(SemanticClosureContractValidator.IsValid(document)
            && SemanticClassContractValidator.IsValid(document)
            && SemanticDispatchContractValidator.IsValid(document)
            && SemanticUeMethodCatalogValidator.IsValid(document),
            "Task<int> producer passes all existing semantic readers");
        Assert(!SemanticAsyncInvocationValidator.IsValid(document with
        {
            AsyncMethods = new[] { method with { TaskResultTypeId = "type:float64" } }
        }), "Task result type tampering is rejected");
        Assert(!SemanticAsyncInvocationValidator.IsValid(document with
        {
            SchemaVersion = SemanticContract.CurrentSchemaVersion,
            SemanticVersion = SemanticContract.CurrentSemanticVersion,
        }), "Task result metadata cannot be relabeled as an older contract");
        Assert(!SemanticAsyncInvocationValidator.IsValid(document with { TypeShapes = null! }),
            "missing task type shapes fail closed");
    }

    private static void LocalInitializersPreserveContextualConversions()
    {
        string[] bodies =
        {
            "await AvidContinuations.NextTickAsync(); double value = GetValue(); Consume(value);",
            "await AvidContinuations.NextTickAsync(); if (GetValue() > 0) { double value = GetValue(); Consume(value); }",
            "for (double value = GetValue(); value < 3; value++) { await AvidContinuations.NextTickAsync(); Consume(value); }",
        };
        for (int index = 0; index < bodies.Length; ++index)
        {
            string source = $$"""
                using AvidScript;
                using System.Runtime.InteropServices;
                namespace Game;
                public static class Script
                {
                    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                    public static async void BeginPlay() { {{bodies[index]}} }
                    private static int GetValue() => 1;
                    private static void Consume(double value) { }
                }
                """;
            SemanticDocument document = Analyze(source, $"Scripts/AsyncInitializer{index}.cs");
            Assert(document.Succeeded, "async initializer source must analyze successfully");
            SemanticAsyncMethod method = document.AsyncMethods.Single();
            SemanticOperation[] roots = method.Segments.SelectMany(segment => segment.Statements)
                .Select(statement => statement.Operation).ToArray();
            SemanticOperation initializer = index == 0
                ? method.Segments.SelectMany(segment => segment.Statements)
                    .Single(statement => statement.TargetSymbolId is not null).Operation
                : roots.SelectMany(Enumerate).Single(operation => operation.Kind
                    == SemanticAsyncMethod.LocalDeclarationOperationKind).Children.Single();
            Assert(initializer.Kind == "conversion"
                && initializer.TypeId == "type:float64"
                && initializer.Children.Single().TypeId == "type:int32",
                "CPS, structured flow, and CFG for declarations must retain the Roslyn int-to-double conversion");
        }
    }

    private static void EarlyReturnGuardsProjectStableControlOperations()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [AvidExport("guarded")]
                public static async void Guarded()
                {
                    await AvidContinuations.NextTickAsync();
                    if (ShouldStop())
                    {
                        return;
                    }
                    await AvidContinuations.NextTickAsync();
                }

                private static bool ShouldStop() => false;
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/GuardedAsync.cs");
        SemanticAsyncMethod method = document.AsyncMethods.Single();
        SemanticAsyncStatement guard = method.Segments[1].Statements.Single();

        Assert(document.Succeeded
            && document.SchemaVersion == 31
            && document.SemanticVersion == "1.40"
            && method.Segments.Count == 3
            && guard.TargetSymbolId is null
            && guard.Operation.Kind == SemanticAsyncMethod.EarlyReturnGuardOperationKind
            && guard.Operation.TypeId == "type:void"
            && guard.Operation.Children.Count == 1
            && guard.Operation.Children[0].TypeId == "type:bool",
            "top-level early-return guards should remain explicit under the schema-v31 semantic-1.40 contract");

        const string invalidSource = """
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [AvidExport("invalid_guard")]
                public static async void InvalidGuard()
                {
                    await AvidContinuations.NextTickAsync();
                    if (ShouldStop())
                    {
                        Consume();
                    }
                    else
                    {
                        Consume();
                    }
                }

                private static bool ShouldStop() => false;
                private static void Consume() { }
            }
            """;
        SemanticDocument branched = Analyze(invalidSource, "Scripts/BranchedAsync.cs");
        SemanticOperation branch = branched.AsyncMethods.Single()
            .Segments[1]
            .Statements.Single()
            .Operation;
        Assert(branched.Succeeded
            && branch.Kind == SemanticAsyncMethod.IfOperationKind
            && branch.Children.Count == 3
            && branch.Children.Skip(1).All(child =>
                child.Kind == SemanticAsyncMethod.BlockOperationKind),
            "if/else side effects should project as explicit structured async flow");
    }

    private static void GeneratedLatentProducerProjectsImportIdentity()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [AvidExport("avid_on_begin_play")]
                public static async void BeginPlay()
                {
                    await UKismetSystemLibrary.DelayAsync(0.125f);
                    await UKismetSystemLibrary.WaitForFlagAsync(true);
                    await UKismetSystemLibrary.WaitForModeAsync();
                    await UKismetSystemLibrary.WaitForTargetAsync(default);
                    await UKismetSystemLibrary.WaitForLocationAsync(new FVector(1.0f, 2.0f, 3.0f));
                    await UKismetSystemLibrary.WaitForSettingsAsync(default);
                }
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/GeneratedLatentAsync.cs");
        SemanticAsyncAwaitSite[] sites = document.AsyncMethods.Single()
            .Segments.Where(segment => segment.AwaitSite is not null)
            .Select(segment => segment.AwaitSite!)
            .ToArray();
        SemanticAsyncAwaitSite delaySite = sites.Single(site =>
            site.ProducerKind == "binding_latent|avidscript|avid_ue_latent_test");
        SemanticAsyncAwaitSite boolSite = sites.Single(site =>
            site.ProducerKind == "binding_latent|avidscript|avid_ue_latent_bool_test");
        SemanticAsyncAwaitSite enumSite = sites.Single(site =>
            site.ProducerKind == "binding_latent|avidscript|avid_ue_latent_enum_test");
        SemanticAsyncAwaitSite objectSite = sites.Single(site =>
            site.ProducerKind == "binding_latent|avidscript|avid_ue_latent_object_test");
        SemanticAsyncAwaitSite vectorSite = sites.Single(site =>
            site.ProducerKind == "binding_latent|avidscript|avid_ue_latent_vector_test");
        SemanticAsyncAwaitSite wireSite = sites.Single(site =>
            site.ProducerKind == "binding_latent|avidscript|avid_ue_latent_wire_test");
        SemanticCallable delayImport = document.Callables.Single(callable =>
            callable.Import is { Module: "avidscript", Name: "avid_ue_latent_test" });
        SemanticCallable boolImport = document.Callables.Single(callable =>
            callable.Import is { Module: "avidscript", Name: "avid_ue_latent_bool_test" });
        SemanticCallable enumImport = document.Callables.Single(callable =>
            callable.Import is { Module: "avidscript", Name: "avid_ue_latent_enum_test" });
        SemanticCallable objectImport = document.Callables.Single(callable =>
            callable.Import is { Module: "avidscript", Name: "avid_ue_latent_object_test" });
        SemanticCallable vectorImport = document.Callables.Single(callable =>
            callable.Import is { Module: "avidscript", Name: "avid_ue_latent_vector_test" });
        SemanticCallable wireImport = document.Callables.Single(callable =>
            callable.Import is { Module: "avidscript", Name: "avid_ue_latent_wire_test" });

        Assert(document.Succeeded
            && delaySite.CallbackId == CompilerCallbackIdStart
            && delaySite.PayloadKind == "none"
            && delaySite.Arguments.Count == 1
            && delaySite.Arguments[0].TypeId == "type:float32"
            && delayImport.ReturnTypeId == "type:int64"
            && delayImport.Parameters.Select(parameter => parameter.TypeId)
                .SequenceEqual(new[] { "type:float32", "type:int32" }),
            "generated latent markers should project a generic import identity and compiler callback ABI");
        Assert(boolSite.CallbackId == CompilerCallbackIdStart + 1
            && boolSite.PayloadKind == "none"
            && boolSite.Arguments.Count == 1
            && boolSite.Arguments[0].TypeId == "type:bool"
            && boolImport.ReturnTypeId == "type:int64"
            && boolImport.Parameters.Select(parameter => parameter.TypeId)
                .SequenceEqual(new[] { "type:int32", "type:int32" }),
            "generated boolean latent producers should preserve public bool and import i32 storage identities");
        Assert(enumSite.CallbackId == CompilerCallbackIdStart + 2
            && enumSite.PayloadKind == "none"
            && enumSite.Arguments.Count == 1
            && enumSite.Arguments[0].TypeId is { } enumArgumentTypeId
            && enumArgumentTypeId.EndsWith(
                "EAvidScriptCSharpEmitterTestMode",
                StringComparison.Ordinal)
            && enumImport.ReturnTypeId == "type:int64"
            && enumImport.Parameters.Select(parameter => parameter.TypeId)
                .SequenceEqual(new[] { "type:int32", "type:int32" }),
            "generated enum latent producers should materialize the default enum and retain i32 import storage");
        Assert(objectSite.CallbackId == CompilerCallbackIdStart + 3
            && objectSite.Arguments.Count == 1
            && objectSite.Arguments[0].TypeId?.EndsWith("UObject", StringComparison.Ordinal) == true
            && objectImport.Parameters.Select(parameter => parameter.TypeId)
                .SequenceEqual(new[] { "type:int32", "type:int32", "type:int32" }),
            "generated object latent producers should retain one public capability and two import cells");
        Assert(vectorSite.CallbackId == CompilerCallbackIdStart + 4
            && vectorSite.Arguments.Count == 1
            && vectorSite.Arguments[0].TypeId?.EndsWith("FVector", StringComparison.Ordinal) == true
            && vectorImport.Parameters.Select(parameter => parameter.TypeId)
                .SequenceEqual(new[] { "type:float32", "type:float32", "type:float32", "type:int32" }),
            "generated vector latent producers should retain one public value and three import cells");
        Assert(wireSite.CallbackId == CompilerCallbackIdStart + 5
            && wireSite.Arguments.Count == 1
            && wireImport.Parameters.Count == 2
            && wireImport.Parameters[0].RefKind == "in"
            && wireImport.Parameters[0].TypeId == wireSite.Arguments[0].TypeId
            && wireImport.Parameters[1].TypeId == "type:int32",
            "generated struct-wire latent producers should retain one public value and one address import");
    }

    private static void SequentialAwaitsProjectStableSegments()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using AvidScript;

            namespace Game;

            public static class Script
            {
                private const string CubePath = "/Engine/EngineMeshes/Cube.Cube";

                [AvidExport("avid_on_begin_play")]
                public static async void BeginPlay()
                {
                    float delay = 0.25f;
                    ConsumeDelay(delay);
                    await AvidContinuations.DelayAsync(GetDelay(delay));
                    NativeAfterAwait(7);
                    await AvidContinuations.NextTickAsync();
                    AvidLoadedObject loaded = await AvidAssets.LoadObjectAsync(CubePath);
                    Consume(loaded);
                }

                [AvidExport("avid_on_end_play")]
                public static async void EndPlay()
                {
                    await AvidContinuations.NextTickAsync();
                }

                private static void ConsumeDelay(float value) { }
                private static float GetDelay(float value) => value;
                private static void Consume(AvidLoadedObject value) { }

                [DllImport("env", EntryPoint = "async_after_await")]
                private static extern void NativeAfterAwait(int value);
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/ControlledAsync.cs");
        SemanticAsyncMethod beginPlay = document.AsyncMethods.Single(method =>
            method.ExportName == "avid_on_begin_play");
        SemanticAsyncMethod endPlay = document.AsyncMethods.Single(method =>
            method.ExportName == "avid_on_end_play");
        SemanticAsyncAwaitSite[] beginAwaits = beginPlay.Segments
            .Select(segment => segment.AwaitSite)
            .Where(site => site is not null)
            .Cast<SemanticAsyncAwaitSite>()
            .ToArray();

        Assert(document.Succeeded
            && document.SchemaVersion == 31
            && document.SemanticVersion == "1.40"
            && document.AsyncMethods.Count == 2,
            "controlled async exports should publish schema 31 / semantic 1.40");
        string oldContractJson = Encoding.UTF8.GetString(SemanticSerializer.Serialize(document));
        Assert(!oldContractJson.Contains("task_result_type_id", StringComparison.Ordinal)
            && !oldContractJson.Contains("task_callable_id", StringComparison.Ordinal),
            "old async void artifacts omit the new task fields");
        Assert(!SemanticAsyncInvocationValidator.IsValid(document with
        {
            SchemaVersion = SemanticContract.TaskResultSchemaVersion,
            SemanticVersion = SemanticContract.TaskResultSemanticVersion,
        }), "old async void artifacts cannot claim the task-result contract");
        Assert(beginPlay.Lowering == "reentrant_zero_heap_cps"
            && beginPlay.Segments.Select(segment => segment.Ordinal)
                .SequenceEqual(new[] { 0, 1, 2, 3 })
            && beginAwaits.Select(site => site.CallbackId).SequenceEqual(new[]
            {
                CompilerCallbackIdStart,
                CompilerCallbackIdStart + 1,
                CompilerCallbackIdStart + 2,
            })
            && beginAwaits.Select(site => site.ProducerKind)
                .SequenceEqual(new[] { "delay", "next_tick", "object_load" })
            && beginAwaits.Select(site => site.PayloadKind)
                .SequenceEqual(new[] { "none", "none", "object" }),
            "await sites should preserve source order, producer shape, and compiler callback allocation");
        Assert(beginPlay.Segments[0].Statements.Count == 2
            && beginPlay.Segments[0].Statements[0].TargetSymbolId is not null
            && beginPlay.Segments[0].Statements[0].Operation.Kind == "literal"
            && beginPlay.Segments[3].Statements.Single().Operation.Kind == "expression_statement",
            "ordinary segment statements should project initializer values and optional local targets");
        Assert(beginAwaits[2].ResultSymbolId is not null
            && beginAwaits[2].ResultTypeId == "type:global::AvidScript.AvidLoadedObject"
            && beginAwaits[2].Arguments.Single().Constant?.Value ==
                "/Engine/EngineMeshes/Cube.Cube",
            "object awaits should publish their constant path and immediately resumed result local");
        Assert(endPlay.Segments[0].AwaitSite?.CallbackId == CompilerCallbackIdStart + 3
            && !document.ControlFlowGraphs.Any(graph =>
                graph.MethodSymbolId == beginPlay.MethodSymbolId
                || graph.MethodSymbolId == endPlay.MethodSymbolId),
            "compiler callback ids should continue across exports and async methods should not publish CFGs");
        Assert(document.Reachability?.ReachableImports.Single().Name == "async_after_await"
            && document.Reachability.ReachableCallableIds.Any(id =>
                id.Contains(".Consume(", StringComparison.Ordinal))
            && document.Reachability.ReachableCallableIds.Any(id =>
                id.Contains(".GetDelay(", StringComparison.Ordinal)),
            "resume statements and await arguments should keep direct imports and synchronous callees reachable");
        SemanticOperation asyncRoot = document.Methods.Single(method =>
            method.MethodSymbolId == beginPlay.MethodSymbolId).Root;
        Assert(Enumerate(asyncRoot).Any(operation => operation.Kind == "await"),
            "ordinary operation projection should expose a stable await kind");

        string json = Encoding.UTF8.GetString(SemanticSerializer.Serialize(document));
        Assert(json.Contains("\"async_methods\"", StringComparison.Ordinal)
            && json.Contains("\"result_type_id\": \"type:global::AvidScript.AvidLoadedObject\"", StringComparison.Ordinal)
            && json.IndexOf("\"continuation_callbacks\"", StringComparison.Ordinal)
                < json.IndexOf("\"async_methods\"", StringComparison.Ordinal)
            && json.IndexOf("\"async_methods\"", StringComparison.Ordinal)
                < json.IndexOf("\"diagnostics\"", StringComparison.Ordinal),
            "async graph serialization should be stable and precede diagnostics");
    }

    private static void CrossBoundaryLocalsPublishStateFrames()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [AvidExport("cross_state")]
                public static async void CrossState()
                {
                    int count = 1;
                    FVector offset = new FVector(1.0f, 2.0f, 3.0f);
                    await AvidContinuations.NextTickAsync();
                    AvidLoadedObject loaded = await AvidAssets.LoadObjectAsync("/Game/Valid.Valid");
                    await AvidContinuations.NextTickAsync();
                    Consume(count);
                    Consume(offset);
                    Consume(loaded);
                }

                private static void Consume(int value) { }
                private static void Consume(FVector value) { }
                private static void Consume(AvidLoadedObject value) { }
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/CrossAwaitLocals.cs");

        SemanticAsyncAwaitSite[] sites = document.AsyncMethods.Single().Segments
            .Where(segment => segment.AwaitSite is not null)
            .Select(segment => segment.AwaitSite!)
            .ToArray();
        string countId = document.Symbols.Single(symbol => symbol.Kind == "local" && symbol.Name == "count").Id;
        string offsetId = document.Symbols.Single(symbol => symbol.Kind == "local" && symbol.Name == "offset").Id;
        string loadedId = document.Symbols.Single(symbol => symbol.Kind == "local" && symbol.Name == "loaded").Id;

        Assert(document.Succeeded
            && document.AsyncMethods.Count == 1
            && sites.Length == 3
            && sites[0].StateFrame?.Slots.Select(slot => slot.SymbolId)
                .SequenceEqual(new[] { countId, offsetId }.OrderBy(id => id, StringComparer.Ordinal)) == true
            && sites[1].StateFrame?.Slots.Select(slot => slot.SymbolId)
                .SequenceEqual(new[] { countId, offsetId }.OrderBy(id => id, StringComparer.Ordinal)) == true
            && sites[2].StateFrame?.Slots.Select(slot => slot.SymbolId)
                .SequenceEqual(new[] { countId, loadedId, offsetId }.OrderBy(id => id, StringComparer.Ordinal)) == true
            && sites.All(site => site.StateFrame?.TypeId
                == $"type:synthetic:async_state:{site.CallbackId}"),
            "scalar, fixed struct, and object-handle locals should publish exact per-await state frames");
    }

    private static void StructuredFlowProjectsExactStateFrames()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [AvidExport("structured")]
                public static async void Structured()
                {
                    int movementCount;
                    if (ShouldDouble())
                    {
                        movementCount = 2;
                    }
                    else
                    {
                        movementCount = 1;
                    }

                    int overwritten = 4;
                    await AvidContinuations.NextTickAsync();

                    overwritten = 9;
                    int total = movementCount;
                    for (int index = 0; index < 3; ++index)
                    {
                        if (index == 1)
                        {
                            continue;
                        }
                        total += index;
                    }
                    while (total < 8)
                    {
                        ++total;
                        if (total == 7)
                        {
                            break;
                        }
                    }
                    do
                    {
                        --total;
                    }
                    while (total > 6);
                    Consume(overwritten + total);
                }

                private static bool ShouldDouble() => true;
                private static void Consume(int value) { }
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/StructuredAsync.cs");
        SemanticAsyncMethod method = document.AsyncMethods.Single();
        SemanticAsyncAwaitSite awaitSite = method.Segments[0].AwaitSite!;
        string movementCountId = document.Symbols.Single(symbol =>
            symbol.Kind == "local" && symbol.Name == "movementCount").Id;
        string overwrittenId = document.Symbols.Single(symbol =>
            symbol.Kind == "local" && symbol.Name == "overwritten").Id;
        SemanticOperation[] flow = method.Segments
            .SelectMany(segment => segment.Statements)
            .SelectMany(statement => Enumerate(statement.Operation))
            .ToArray();

        Assert(document.Succeeded
            && document.SchemaVersion == 31
            && document.SemanticVersion == "1.40"
            && awaitSite.StateFrame is { } stateFrame
            && stateFrame.Slots.Select(slot => slot.SymbolId)
                .SequenceEqual(new[] { movementCountId })
            && stateFrame.Slots.All(slot => slot.SymbolId != overwrittenId)
            && flow.Any(operation => operation.Kind == SemanticAsyncMethod.LocalDeclarationOperationKind)
            && flow.Any(operation => operation.Kind == SemanticAsyncMethod.IfOperationKind)
            && flow.Any(operation => operation.Kind == SemanticAsyncMethod.ForOperationKind)
            && flow.Any(operation => operation.Kind == SemanticAsyncMethod.WhileOperationKind)
            && flow.Any(operation => operation.Kind == SemanticAsyncMethod.DoWhileOperationKind)
            && flow.Any(operation => operation.Kind == SemanticAsyncMethod.BreakOperationKind)
            && flow.Any(operation => operation.Kind == SemanticAsyncMethod.ContinueOperationKind),
            "structured async flow should preserve only truly live locals across await boundaries");
    }

    private static void NestedAwaitsProjectContinuationCfg()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [AvidExport("nested_cfg")]
                public static async void NestedCfg()
                {
                    int movementCount = 2;
                    for (int index = 0; index < movementCount; ++index)
                    {
                        await AvidContinuations.NextTickAsync();
                        Consume(index);
                    }

                    if (ShouldDelay(movementCount))
                    {
                        await AvidContinuations.DelayAsync(0.1f);
                    }
                    Consume(movementCount);
                }

                private static bool ShouldDelay(int value) => value > 1;
                private static void Consume(int value) { }
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/NestedAwaitCfg.cs");
        SemanticAsyncMethod method = document.AsyncMethods.Single();
        SemanticAsyncSegment[] awaitSegments = method.Segments
            .Where(segment => segment.AwaitSite is not null)
            .ToArray();
        string movementCountId = document.Symbols.Single(symbol =>
            symbol.Kind == "local" && symbol.Name == "movementCount").Id;
        string indexId = document.Symbols.Single(symbol =>
            symbol.Kind == "local" && symbol.Name == "index").Id;

        Assert(document.Succeeded
            && document.SchemaVersion == 31
            && document.SemanticVersion == "1.40"
            && method.Lowering == SemanticAsyncMethod.ContinuationCfgLowering
            && method.EntrySegmentOrdinal >= 0
            && method.EntrySegmentOrdinal < method.Segments.Count
            && method.Segments.Select(segment => segment.Ordinal)
                .SequenceEqual(Enumerable.Range(0, method.Segments.Count))
            && method.Segments.All(segment => segment.Transfer is not null)
            && method.Segments.Any(segment => segment.Transfer?.Kind
                == SemanticAsyncMethod.BranchTransferKind)
            && method.Segments.Any(segment => segment.Transfer?.Kind
                == SemanticAsyncMethod.GotoTransferKind)
            && method.Segments.Any(segment => segment.Transfer?.Kind
                == SemanticAsyncMethod.ReturnTransferKind)
            && awaitSegments.Length == 2
            && awaitSegments.All(segment => segment.Transfer?.Kind
                == SemanticAsyncMethod.AwaitTransferKind)
            && awaitSegments.Select(segment => segment.AwaitSite!.CallbackId)
                .SequenceEqual(new[] { CompilerCallbackIdStart, CompilerCallbackIdStart + 1 })
            && awaitSegments.All(segment => segment.Transfer!.PrimaryTarget >= 0),
            "nested branch and loop awaits should project one bounded continuation CFG with exact resume targets");
        Assert(document.Reachability!.ReachableCallableIds.Any(id => id.Contains(
                ".ShouldDelay(",
                StringComparison.Ordinal)),
            "continuation CFG branch conditions should retain their synchronous helper call graph");

        SemanticAsyncStateFrame loopFrame = awaitSegments[0].AwaitSite!.StateFrame!;
        SemanticAsyncStateFrame branchFrame = awaitSegments[1].AwaitSite!.StateFrame!;
        Assert(loopFrame.Slots.Select(slot => slot.SymbolId)
                .SequenceEqual(new[] { indexId, movementCountId }.OrderBy(id => id, StringComparer.Ordinal))
            && branchFrame.Slots.Select(slot => slot.SymbolId)
                .SequenceEqual(new[] { movementCountId }),
            "CFG liveness should preserve loop control state and remove locals that are dead at a later branch await");
    }

    private static void SwitchAwaitsProjectContinuationCfg()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            public enum Mode
            {
                Stop,
                Tick,
                Delay,
                AlsoDelay,
            }

            public static class Script
            {
                [AvidExport("switch_cfg")]
                public static async void SwitchCfg()
                {
                    Mode mode = Mode.Delay;
                    switch (mode)
                    {
                        case Mode.Stop:
                            return;
                        case Mode.Tick:
                            await AvidContinuations.NextTickAsync();
                            break;
                        case Mode.Delay:
                        case Mode.AlsoDelay:
                            await AvidContinuations.DelayAsync(0.01f);
                            break;
                        default:
                            await AvidContinuations.NextTickAsync();
                            break;
                    }
                    Consume((int)mode);
                }

                private static void Consume(int value) { }
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/SwitchAwaitCfg.cs");
        SemanticAsyncMethod method = document.AsyncMethods.Single();
        SemanticAsyncSegment[] awaitSegments = method.Segments
            .Where(segment => segment.AwaitSite is not null)
            .OrderBy(segment => segment.AwaitSite!.CallbackId)
            .ToArray();
        string modeId = document.Symbols.Single(symbol =>
            symbol.Kind == "local" && symbol.Name == "mode").Id;

        Assert(document.Succeeded
            && method.Lowering == SemanticAsyncMethod.ContinuationCfgLowering
            && awaitSegments.Length == 3
            && method.Segments.Count(segment => segment.Transfer?.Kind
                == SemanticAsyncMethod.BranchTransferKind) == 4
            && awaitSegments.All(segment => segment.AwaitSite!.StateFrame!.Slots.Any(slot =>
                slot.SymbolId == modeId))
            && method.Segments.Where(segment => segment.Transfer?.Kind
                    == SemanticAsyncMethod.BranchTransferKind)
                .All(segment => segment.Transfer!.Condition is
                {
                    Kind: "binary",
                    OperatorKind: "equals",
                    TypeId: "type:bool",
                }),
            "enum switch sections should project deterministic equality dispatch and exact await state frames");

        const string unstableSource = """
            using AvidScript;
            namespace Game;
            public static class Script
            {
                [AvidExport("unstable_switch")]
                public static async void UnstableSwitch()
                {
                    switch (SelectMode())
                    {
                        case 1:
                            await AvidContinuations.NextTickAsync();
                            break;
                    }
                }

                private static int SelectMode() => 1;
            }
            """;
        SemanticDocument unstable = Analyze(unstableSource, "Scripts/UnstableSwitchAwait.cs");
        Assert(!unstable.Succeeded
            && unstable.AsyncMethods.Count == 0
            && unstable.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS5418"),
            "side-effecting switch governing expressions should fail closed until hidden spill locals are frozen");

        const string synchronousSwitchSource = """
            using AvidScript;
            namespace Game;
            public static class Script
            {
                [AvidExport("synchronous_switch")]
                public static async void SynchronousSwitch()
                {
                    await AvidContinuations.NextTickAsync();
                    int mode = 1;
                    switch (mode)
                    {
                        case 1:
                            Consume(mode);
                            break;
                        default:
                            return;
                    }
                }

                private static void Consume(int value) { }
            }
            """;
        SemanticDocument synchronousSwitch = Analyze(
            synchronousSwitchSource,
            "Scripts/SynchronousSwitchCfg.cs");
        Assert(synchronousSwitch.Succeeded
            && synchronousSwitch.AsyncMethods.Single().Lowering
                == SemanticAsyncMethod.ContinuationCfgLowering,
            "controlled async methods containing synchronous switch dispatch should select continuation CFG lowering");

        const string loopSwitchSource = """
            using AvidScript;
            namespace Game;
            public static class Script
            {
                [AvidExport("loop_switch")]
                public static async void LoopSwitch()
                {
                    int mode = 0;
                    while (mode < 2)
                    {
                        switch (mode)
                        {
                            case 0:
                                ++mode;
                                await AvidContinuations.NextTickAsync();
                                continue;
                            default:
                                break;
                        }
                        break;
                    }
                    Consume(mode);
                }

                private static void Consume(int value) { }
            }
            """;
        SemanticDocument loopSwitch = Analyze(loopSwitchSource, "Scripts/LoopSwitchCfg.cs");
        Assert(loopSwitch.Succeeded
            && loopSwitch.AsyncMethods.Single().Segments.Single(segment =>
                    segment.AwaitSite is not null)
                .AwaitSite!.StateFrame!.Slots.Any(),
            "switch break should exit the switch while continue retains the enclosing loop target across await");
    }

    private static void ArrayForeachAwaitsProjectCompilerState()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [AvidExport("foreach_cfg")]
                public static async void ForeachCfg()
                {
                    int[] values = new[] { 2, 3, 5 };
                    int total = 0;
                    foreach (int value in values)
                    {
                        await AvidContinuations.NextTickAsync();
                        total += value;
                    }
                    Consume(total);
                }

                private static void Consume(int value) { }
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/ArrayForeachAwait.cs");
        SemanticAsyncMethod method = document.AsyncMethods.SingleOrDefault()
            ?? throw new InvalidOperationException(string.Join(
                " | ",
                document.Diagnostics.Select(diagnostic =>
                    $"{diagnostic.Code}@{diagnostic.Span.Start}: {diagnostic.Message}")));
        SemanticAsyncAwaitSite awaitSite = method.Segments.Single(segment =>
            segment.AwaitSite is not null).AwaitSite!;
        SemanticOperation[] operations = method.Segments
            .SelectMany(segment => segment.Statements.Select(statement => statement.Operation)
                .Concat(segment.Transfer?.Condition is { } condition
                    ? new[] { condition }
                    : Array.Empty<SemanticOperation>()))
            .SelectMany(Enumerate)
            .ToArray();
        SemanticAsyncCompilerLocal arrayLocal = method.CompilerLocals.Single(local =>
            local.Name.StartsWith("<foreach_array_", StringComparison.Ordinal));
        SemanticAsyncCompilerLocal indexLocal = method.CompilerLocals.Single(local =>
            local.Name.StartsWith("<foreach_index_", StringComparison.Ordinal));

        Assert(document.Succeeded
            && document.SchemaVersion == 31
            && document.SemanticVersion == "1.40"
            && method.Lowering == SemanticAsyncMethod.ContinuationCfgLowering
            && method.CompilerLocals.Count == 2
            && arrayLocal.TypeId == "type:int32[]"
            && indexLocal.TypeId == "type:int32"
            && awaitSite.StateFrame!.Slots.Any(slot =>
                slot.SymbolId == arrayLocal.SymbolId && slot.TypeId == arrayLocal.TypeId)
            && awaitSite.StateFrame.Slots.Any(slot =>
                slot.SymbolId == indexLocal.SymbolId && slot.TypeId == indexLocal.TypeId)
            && operations.Any(operation => operation is
                {
                    Kind: "property_reference",
                    SymbolId: SemanticIntrinsicIds.ArrayLengthPropertyId,
                })
            && operations.Any(operation => operation.Kind == "array_element_reference")
            && operations.Any(operation => operation is
                {
                    Kind: "increment_or_decrement",
                    OperatorKind: "increment",
                }),
            "array foreach with await should publish deterministic compiler locals and exact resumable CFG state");

        const string conversionSource = """
            using AvidScript;
            namespace Game;
            public static class Script
            {
                [AvidExport("converted_foreach")]
                public static async void ConvertedForeach()
                {
                    int[] values = new[] { 1 };
                    foreach (long value in values)
                    {
                        await AvidContinuations.NextTickAsync();
                    }
                }
            }
            """;
        SemanticDocument converted = Analyze(
            conversionSource,
            "Scripts/ConvertedArrayForeachAwait.cs");
        Assert(!converted.Succeeded
            && converted.AsyncMethods.Count == 0
            && converted.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS5419"),
            "foreach element conversions should remain fail-closed until conversion state is frozen");
    }

    private static void NonFrozenAsyncShapesRemainFailClosed()
    {
        const string source = """
            using System.Threading.Tasks;
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [AvidExport("task_owner")]
                public static async Task TaskOwner()
                {
                    await Task.Yield();
                }

                [AvidExport("nested")]
                public static async void Nested()
                {
                    Consume(await AvidAssets.LoadObjectAsync("/Game/Valid.Valid"));
                }

                [AvidExport("arbitrary")]
                public static async void ArbitraryAwaiter()
                {
                    await CustomProducer.GetAsync();
                }

                private static void Consume(AvidLoadedObject value) { }
            }

            public static class CustomProducer
            {
                public static AvidVoidAwaitable GetAsync() => default;
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/UnsupportedAsync.cs");

        Assert(!document.Succeeded
            && document.AsyncMethods.Count == 0
            && document.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS5401")
            && document.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS5402")
            && document.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS5403")
            && document.Diagnostics.Count(diagnostic => diagnostic.Code == "ASCS3002") == 3,
            "Task owners, expression-nested awaits, and arbitrary awaiters should retain ASCS3002 and stable async diagnostics");
    }

    private static void CallbackRangesAndAwaitLimitsAreEnforced()
    {
        const string reservedCallbackSource = """
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [AvidContinuation(0x40000000)]
                public static void Reserved() { }
            }
            """;
        SemanticDocument reservedCallback = Analyze(
            reservedCallbackSource,
            "Scripts/ReservedCallback.cs");
        Assert(!reservedCallback.Succeeded
            && reservedCallback.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS5409"),
            "user continuation ids should remain below the compiler-owned callback range");

        string tooManyAwaits = string.Join(
            Environment.NewLine,
            Enumerable.Repeat("await AvidContinuations.NextTickAsync();", 17));
        string perMethodSource = $$"""
            using AvidScript;
            namespace Game;
            public static class Script
            {
                [AvidExport("too_many")]
                public static async void TooMany()
                {
                    {{tooManyAwaits}}
                }
            }
            """;
        SemanticDocument perMethod = Analyze(perMethodSource, "Scripts/TooManyAwaits.cs");
        Assert(!perMethod.Succeeded
            && perMethod.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS5407"),
            "one controlled async method should permit at most sixteen awaits");

        string methods = string.Join(
            Environment.NewLine,
            Enumerable.Range(0, 5).Select(index =>
            {
                string awaits = string.Join(
                    Environment.NewLine,
                    Enumerable.Repeat("await AvidContinuations.NextTickAsync();", 13));
                return $$"""
                    [AvidExport("module_{{index}}")] public static async void Method{{index}}()
                    {
                        {{awaits}}
                    }
                    """;
            }));
        string moduleSource = $$"""
            using AvidScript;
            namespace Game;
            public static class Script
            {
                {{methods}}
            }
            """;
        SemanticDocument module = Analyze(moduleSource, "Scripts/ModuleAwaitLimit.cs");
        Assert(!module.Succeeded
            && module.AsyncMethods.Count == 0
            && module.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS5408"),
            "one module should permit at most sixty-four controlled await sites");
    }

    private static void ControlFlowSegmentLimitFailsClosed()
    {
        string trailingStatements = string.Join(
            Environment.NewLine,
            Enumerable.Repeat("Consume();", 63));
        string source = $$"""
            using AvidScript;
            namespace Game;
            public static class Script
            {
                [AvidExport("oversized_cfg")]
                public static async void OversizedControlFlow()
                {
                    while (ShouldContinue())
                    {
                        await AvidContinuations.NextTickAsync();
                    }
                    {{trailingStatements}}
                }

                private static bool ShouldContinue() => false;
                private static void Consume() { }
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/OversizedAsyncCfg.cs");

        Assert(!document.Succeeded
            && document.AsyncMethods.Count == 0
            && document.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS5417"),
            "oversized controlled async CFGs should fail closed with ASCS5417 instead of throwing");
    }

    internal static SemanticDocument Analyze(string source, string sourceId)
    {
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        return SemanticAnalyzer.Analyze(
            source,
            sourceId,
            frontend.Source.Sha256,
            new[]
            {
                new SemanticReferenceSource(
                    AsyncFacade,
                    "generated://AvidScript.Async.generated.cs",
                    true),
            });
    }

    private static System.Collections.Generic.IEnumerable<SemanticOperation> Enumerate(
        SemanticOperation operation)
    {
        yield return operation;
        foreach (SemanticOperation child in operation.Children)
        {
            foreach (SemanticOperation descendant in Enumerate(child))
            {
                yield return descendant;
            }
        }
    }

    private const string AsyncFacade = """
        using System;
        using System.Runtime.CompilerServices;
        using System.Runtime.InteropServices;

        namespace AvidScript;

        [AttributeUsage(AttributeTargets.Method, Inherited = false, AllowMultiple = false)]
        public sealed class AvidExportAttribute : Attribute
        {
            public AvidExportAttribute(string exportName) { }
        }

        [AttributeUsage(AttributeTargets.Method, Inherited = false, AllowMultiple = false)]
        public sealed class AvidContinuationAttribute : Attribute
        {
            public AvidContinuationAttribute(int callbackId) { }
        }

        [AttributeUsage(AttributeTargets.Method, Inherited = false, AllowMultiple = false)]
        public sealed class AvidLatentAttribute : Attribute
        {
            public AvidLatentAttribute(string module, string importName) { }
        }

        public readonly struct AvidContinuation { }

        public readonly struct AvidVoidAwaitable
        {
            public AvidVoidAwaiter GetAwaiter() => default;
        }

        public readonly struct AvidVoidAwaiter : ICriticalNotifyCompletion
        {
            public bool IsCompleted => false;
            public void GetResult() { }
            public void OnCompleted(Action continuation) { }
            public void UnsafeOnCompleted(Action continuation) { }
        }

        public readonly struct AvidDelayAwaitable
        {
            public AvidDelayAwaiter GetAwaiter() => default;
        }

        public readonly struct AvidDelayAwaiter : ICriticalNotifyCompletion
        {
            public bool IsCompleted => false;
            public void GetResult() { }
            public void OnCompleted(Action continuation) { }
            public void UnsafeOnCompleted(Action continuation) { }
        }

        public readonly struct AvidObjectAwaitable
        {
            public AvidObjectAwaiter GetAwaiter() => default;
        }

        public readonly struct AvidObjectAwaiter : ICriticalNotifyCompletion
        {
            public bool IsCompleted => false;
            public AvidLoadedObject GetResult() => default;
            public void OnCompleted(Action continuation) { }
            public void UnsafeOnCompleted(Action continuation) { }
        }

        public readonly struct AvidLoadedObject { }

        public static class AvidContinuations
        {
            public static AvidContinuation Delay(float delaySeconds, int callbackId) => default;
            public static AvidContinuation NextTick(int callbackId) => default;
            public static AvidDelayAwaitable DelayAsync(float delaySeconds) => default;
            public static AvidDelayAwaitable NextTickAsync() => default;
        }

        public static class AvidAssets
        {
            public static AvidContinuation LoadObjectAsync(string assetPath, int callbackId) => default;
            public static AvidObjectAwaitable LoadObjectAsync(string assetPath) => default;
        }

        public static class UKismetSystemLibrary
        {
            [AvidLatent("avidscript", "avid_ue_latent_test")]
            public static AvidDelayAwaitable DelayAsync(float Duration) => default;

            [AvidLatent("avidscript", "avid_ue_latent_bool_test")]
            public static AvidDelayAwaitable WaitForFlagAsync(bool bExpected) => default;

            [AvidLatent("avidscript", "avid_ue_latent_enum_test")]
            public static AvidDelayAwaitable WaitForModeAsync(
                EAvidScriptCSharpEmitterTestMode Mode = EAvidScriptCSharpEmitterTestMode.Primary) => default;

            [AvidLatent("avidscript", "avid_ue_latent_object_test")]
            public static AvidDelayAwaitable WaitForTargetAsync(UObject Target) => default;

            [AvidLatent("avidscript", "avid_ue_latent_vector_test")]
            public static AvidDelayAwaitable WaitForLocationAsync(FVector Location) => default;

            [AvidLatent("avidscript", "avid_ue_latent_wire_test")]
            public static AvidDelayAwaitable WaitForSettingsAsync(
                FAvidScriptStructWireRootTestType Settings) => default;
        }

        public enum EAvidScriptCSharpEmitterTestMode : int
        {
            Primary,
            Secondary,
        }

        public readonly struct UObject
        {
            private readonly int Slot;
            private readonly int Generation;
            internal int AvidScriptSlot => Slot;
            internal int AvidScriptGeneration => Generation;
        }

        public readonly struct FVector
        {
            public readonly float X;
            public readonly float Y;
            public readonly float Z;
            public FVector(float x, float y, float z)
            {
                X = x;
                Y = y;
                Z = z;
            }
        }

        public struct FAvidScriptStructWireRootTestType
        {
            public int Count;
        }

        internal static class AvidScriptNative
        {
            [DllImport("avidscript", EntryPoint = "avid_ue_latent_test")]
            internal static extern long InvokeLatent(float duration, int callbackId);

            [DllImport("avidscript", EntryPoint = "avid_ue_latent_bool_test")]
            internal static extern long InvokeBooleanLatent(int expected, int callbackId);

            [DllImport("avidscript", EntryPoint = "avid_ue_latent_enum_test")]
            internal static extern long InvokeEnumLatent(int mode, int callbackId);

            [DllImport("avidscript", EntryPoint = "avid_ue_latent_object_test")]
            internal static extern long InvokeObjectLatent(int slot, int generation, int callbackId);

            [DllImport("avidscript", EntryPoint = "avid_ue_latent_vector_test")]
            internal static extern long InvokeVectorLatent(float x, float y, float z, int callbackId);

            [DllImport("avidscript", EntryPoint = "avid_ue_latent_wire_test")]
            internal static extern long InvokeWireLatent(
                in FAvidScriptStructWireRootTestType settings,
                int callbackId);
        }
        """;

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
