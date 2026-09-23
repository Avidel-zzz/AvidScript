using System;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpSemantic;

internal static class SemanticExceptionFlowTests
{
    private const string NestedSource = """
        using System;
        class Script
        {
            static int Run(int value)
            {
                try
                {
                    try
                    {
                        if (value < 0) throw new InvalidOperationException();
                        return value;
                    }
                    finally { value += 1; }
                }
                catch (InvalidOperationException) { throw; }
                finally { value += 2; }
            }
        }
        """;

    public static int Run()
    {
        NestedThrowCatchRethrowKeepsRoslynRegions();
        ExpressionThrowKeepsItsSourceSpan();
        CatchFilterIsExplicitlyRejected();
        NestedHandlersKeepTheirOwnRegions();
        CatchAllKeepsItsRegion();
        CrossMethodCallGetsCatchRoute();
        NestedCallableOwnsItsThrowSite();
        OrdinaryArtifactsKeepTheirOriginalShape();
        ContractValidatorRejectsDowngradeAndCorruption();
        return 9;
    }

    private static void NestedThrowCatchRethrowKeepsRoslynRegions()
    {
        SemanticDocument document = Analyze(NestedSource, "Scripts/ExceptionFlow.cs");
        Check(!document.Succeeded && document.SchemaVersion == 33
            && document.SemanticVersion == "1.42", "exception plans require a separate failed Semantic identity");
        Check(document.ControlFlowGraphs.Count == 0
            && document.Diagnostics.Any(item => item.Code == "ASCS3001"),
            "exception source must not publish an executable CFG");
        SemanticExceptionFlow flow = document.ExceptionFlows!.Single();
        Check(flow.MethodSymbolId.Contains(".Run(", StringComparison.Ordinal),
            "exception plan must identify its owning method");
        Check(flow.SourceId == "Scripts/ExceptionFlow.cs" && flow.SourceLength == NestedSource.Length,
            "exception spans must be tied to their compilation unit");
        Check(flow.Throws.Select(site => site.Kind).SequenceEqual(new[] { "throw", "rethrow" }),
            "throw and rethrow need separate source sites");
        Check(flow.Throws[0].ExceptionTypeId == "type:global::System.InvalidOperationException"
            && flow.Throws[1].ExceptionTypeId is null,
            "throw must retain its exception type while rethrow uses the active handler");
        Check(flow.Catches.Count == 1
            && flow.Catches[0].ExceptionTypeId == flow.Throws[0].ExceptionTypeId
            && flow.Regions[flow.Catches[0].RegionOrdinal].Kind == "catch"
            && !flow.Catches[0].HasFilter,
            "typed catch must retain type matching and filter status");
        Check(flow.Regions.Any(region => region.Kind == "catch")
            && flow.Regions.Count(region => region.Kind == "finally") >= 2,
            "nested catch and cleanup regions must survive projection");
        Check(flow.Branches.Any(branch => branch.Semantics is "throw" or "rethrow")
            && flow.Branches.Any(branch => branch.FinallyRegionOrdinals.Count > 0),
            "Roslyn exception and cleanup branch effects must survive projection");
        Check(flow.Blocks is { Count: > 0 }
            && flow.Blocks.Count == flow.Regions[0].LastBlockOrdinal + 1
            && flow.Blocks[0].Kind == "entry" && flow.Blocks[^1].Kind == "exit"
            && flow.Blocks.Any(block => block.BranchValue is not null
                && ContainsOperation(block.BranchValue, "object_creation"))
            && flow.Blocks.Any(block => block.Operations.Any(operation =>
                ContainsOperation(operation, "compound_assignment"))),
            "exception blocks must retain throw values and cleanup operations for later lowering");
        Check(flow.Blocks!.All(block => block.EnclosingRegionOrdinal >= 0
            && flow.Regions[block.EnclosingRegionOrdinal].FirstBlockOrdinal <= block.Ordinal
            && flow.Regions[block.EnclosingRegionOrdinal].LastBlockOrdinal >= block.Ordinal),
            "each exception block must retain its Roslyn region ownership");
        Check(SemanticExceptionDispatchPlanner.TryBuild(flow, out SemanticExceptionDispatchPlan? dispatch)
            && dispatch is not null,
            "nested exception regions must produce a bounded dispatch plan");
        SemanticExceptionDispatchRoute throwRoute = dispatch!.Routes.Single(route =>
            route.SourceBlockOrdinal == flow.Branches.Single(branch =>
                branch.Semantics == "throw").SourceBlockOrdinal);
        Check(throwRoute.Steps.Select(step => step.Kind).SequenceEqual(
                new[] { "finally", "catch", "finally" })
            && throwRoute.Steps[1].HandlerOrdinals.SequenceEqual(new[] { 0 }),
            "throw must run inner cleanup before the handler and outer cleanup if unmatched");
        SemanticExceptionDispatchRoute rethrowRoute = dispatch.Routes.Single(route =>
            route.SourceBlockOrdinal == flow.Branches.Single(branch =>
                branch.Semantics == "rethrow").SourceBlockOrdinal);
        Check(rethrowRoute.Steps.Select(step => step.Kind).SequenceEqual(new[] { "finally" }),
            "rethrow must skip its current catch and continue into the outer cleanup");
        int innerFinally = throwRoute.Steps[0].RegionOrdinal;
        int outerFinally = throwRoute.Steps[2].RegionOrdinal;
        Check(dispatch.Routes[flow.Regions[innerFinally].FirstBlockOrdinal].Steps
                .Select(step => step.Kind).SequenceEqual(new[] { "catch", "finally" })
            && dispatch.Routes[flow.Regions[outerFinally].FirstBlockOrdinal].Steps.Count == 0,
            "a replacement error in a finally must continue outward without rerunning that finally");
        Check(flow.Regions.Select(region => region.Ordinal)
            .SequenceEqual(Enumerable.Range(0, flow.Regions.Count)),
            "region ordinals must be stable and contiguous");
        Check(flow.Regions.Skip(1).All(region => region.ParentOrdinal >= 0
            && region.ParentOrdinal < region.Ordinal),
            "regions must point to an earlier enclosing region");
        byte[] first = SemanticSerializer.Serialize(document);
        byte[] second = SemanticSerializer.Serialize(Analyze(NestedSource, "Scripts/ExceptionFlow.cs"));
        Check(first.SequenceEqual(second)
            && first.SequenceEqual(SemanticSerializer.Serialize(SemanticSerializer.Deserialize(first))),
            "exception diagnostic artifacts must serialize deterministically");
    }

    private static void ExpressionThrowKeepsItsSourceSpan()
    {
        const string source = "class Script { int Fail() => throw new System.Exception(); }";
        SemanticDocument document = Analyze(source, "Scripts/ExpressionThrow.cs");
        SemanticThrowSite site = document.ExceptionFlows!.Single().Throws.Single();
        Check(site.Kind == "throw" && site.ExceptionTypeId == "type:global::System.Exception",
            "expression-bodied throw must retain its type");
        Check(source.Substring(site.Span.Start, site.Span.Length)
            == "throw new System.Exception()",
            "throw span must address the original source expression");
    }

    private static void CatchFilterIsExplicitlyRejected()
    {
        const string source = """
            using System;
            class Script
            {
                static void Run(int value)
                {
                    try { throw new Exception(); }
                    catch (Exception) when (value > 0) { }
                }
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/FilteredCatch.cs");
        Check(!document.Succeeded && document.ExceptionFlows!.Single().Catches.Single().HasFilter,
            "filtered catch must remain diagnostic-only");
        Check(!SemanticExceptionDispatchPlanner.TryBuild(document.ExceptionFlows!.Single(), out _),
            "filtered handlers must not acquire an executable dispatch route");
        SemanticDiagnostic diagnostic = document.Diagnostics.Single(item => item.Code == "ASCS3005");
        Check(source.Substring(diagnostic.Span.Start, diagnostic.Span.Length) == "when (value > 0)",
            "unsupported filter diagnostic must point at the filter");
    }

    private static void NestedHandlersKeepTheirOwnRegions()
    {
        const string source = """
            using System;
            class Script
            {
                static void Run(int value)
                {
                    try
                    {
                        if (value == 0) throw new ArgumentException();
                        try { throw new InvalidOperationException(); }
                        catch (InvalidOperationException) { value += 1; }
                    }
                    catch (ArgumentException) { value += 2; }
                    catch (InvalidOperationException) { value += 3; }
                    catch (Exception) { value += 4; }
                }
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/NestedHandlers.cs");
        SemanticExceptionFlow flow = document.ExceptionFlows!.Single();
        Check(flow.Catches.Count == 4
            && flow.Catches.Select(handler => handler.ExceptionTypeId).SequenceEqual(new[]
            {
                "type:global::System.InvalidOperationException",
                "type:global::System.ArgumentException",
                "type:global::System.InvalidOperationException",
                "type:global::System.Exception",
            })
            && flow.Catches.Select(handler => handler.RegionOrdinal).Distinct().Count() == 4
            && RegionDepth(flow, flow.Catches[0].RegionOrdinal)
                > RegionDepth(flow, flow.Catches[2].RegionOrdinal)
            && flow.Catches.All(handler => flow.Regions[handler.RegionOrdinal].Kind == "catch"
                && flow.Regions[handler.RegionOrdinal].ExceptionTypeId == handler.ExceptionTypeId),
            "nested typed handlers must map to their own Roslyn catch regions");
        Check(SemanticExceptionFlowContractValidator.IsValid(document),
            "multiple handler regions must satisfy the versioned contract");
        Check(SemanticExceptionDispatchPlanner.TryBuild(flow, out SemanticExceptionDispatchPlan? plan)
            && plan is not null,
            "nested handlers must produce a dispatch route");
        SemanticThrowSite innerThrow = flow.Throws[1];
        SemanticExceptionBlock throwBlock = flow.Blocks!.Single(block =>
            block.BranchValue is { } value
            && value.Span.Start >= innerThrow.Span.Start
            && value.Span.Start < innerThrow.Span.Start + innerThrow.Span.Length);
        Check(plan!.Routes[throwBlock.Ordinal].Steps.Select(step => step.Kind)
                .SequenceEqual(new[] { "catch", "catch" })
            && plan.Routes[throwBlock.Ordinal].Steps[0].HandlerOrdinals.SequenceEqual(new[] { 0 })
            && plan.Routes[throwBlock.Ordinal].Steps[1].HandlerOrdinals.SequenceEqual(new[] { 1, 2, 3 }),
            "inner throw must try its local handler before outer handlers in source order");
    }

    private static void CatchAllKeepsItsRegion()
    {
        const string source = """
            using System;
            class Script
            {
                static void Run()
                {
                    try { throw new Exception(); }
                    catch { }
                }
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/CatchAll.cs");
        Check(document.ExceptionFlows is not null, string.Join(" | ", document.Diagnostics.Select(diagnostic =>
            $"{diagnostic.Code}: {diagnostic.Message}")));
        SemanticExceptionFlow flow = document.ExceptionFlows!.Single();
        SemanticCatchHandler handler = flow.Catches.Single();
        Check(handler.ExceptionTypeId is null
            && flow.Regions[handler.RegionOrdinal].Kind == "catch"
            && flow.Regions[handler.RegionOrdinal].ExceptionTypeId == "type:object"
            && SemanticExceptionFlowContractValidator.IsValid(document),
            "catch-all must keep an untyped handler tied to its catch region");
        Check(SemanticExceptionDispatchPlanner.TryBuild(flow, out SemanticExceptionDispatchPlan? plan)
            && plan!.Routes.Any(route => route.Steps.Any(step =>
                step.Kind == "catch" && step.HandlerOrdinals.SequenceEqual(new[] { 0 }))),
            "catch-all must remain in the dispatch candidate list");
    }

    private static void CrossMethodCallGetsCatchRoute()
    {
        const string source = """
            using System;
            class Script
            {
                static int Fail() { throw new InvalidOperationException(); }
                static int Run()
                {
                    try { return Fail(); }
                    catch (InvalidOperationException) { return 7; }
                }
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/CrossMethodException.cs");
        SemanticExceptionFlow caller = document.ExceptionFlows!.Single(flow =>
            flow.MethodSymbolId.Contains(".Run(", StringComparison.Ordinal));
        Check(SemanticExceptionDispatchPlanner.TryBuild(caller, out SemanticExceptionDispatchPlan? plan)
            && plan is not null,
            "caller with a catch must yield a dispatch plan for failed callees");
        SemanticExceptionBlock callBlock = caller.Blocks!.Single(block =>
            block.BranchValue is { } value && ContainsOperation(value, "invocation"));
        Check(plan!.Routes[callBlock.Ordinal].Steps.Select(step => step.Kind)
                .SequenceEqual(new[] { "catch" })
            && plan.Routes[callBlock.Ordinal].Steps[0].HandlerOrdinals.SequenceEqual(new[] { 0 }),
            "a language error returned by a call must reach the caller's local handler");
        SemanticExceptionFlow callee = document.ExceptionFlows!.Single(flow =>
            flow.MethodSymbolId.Contains(".Fail(", StringComparison.Ordinal));
        Check(SemanticExceptionDispatchPlanner.TryBuild(callee, out SemanticExceptionDispatchPlan? calleePlan)
            && calleePlan!.Routes.All(route => route.Steps.Count == 0),
            "an uncaught callee error must leave the method for its caller");
    }

    private static void NestedCallableOwnsItsThrowSite()
    {
        const string source = """
            using System;
            class Script
            {
                static void Run()
                {
                    void Local() { throw new Exception(); }
                    Local();
                }
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/NestedThrow.cs");
        Check(document.ExceptionFlows is { Count: 1 }
            && document.ExceptionFlows[0].Throws.Count == 1,
            "nested throw must belong only to its local function");
    }

    private static void OrdinaryArtifactsKeepTheirOriginalShape()
    {
        const string source = "class Script { static int Run() => 7; }";
        SemanticDocument document = Analyze(source, "Scripts/Ordinary.cs");
        Check(document.Succeeded && document.SchemaVersion == 31
            && document.SemanticVersion == "1.40" && document.ExceptionFlows is null,
            "ordinary source must retain the current executable contract");
        string json = System.Text.Encoding.UTF8.GetString(SemanticSerializer.Serialize(document));
        Check(!json.Contains("exception_flows", StringComparison.Ordinal),
            "old Semantic artifacts must not gain a new serialized field");
        Check(SemanticExceptionFlowContractValidator.IsValid(document),
            "ordinary Semantic artifacts must remain valid without exception flow metadata");
    }

    private static void ContractValidatorRejectsDowngradeAndCorruption()
    {
        SemanticDocument document = Analyze(NestedSource, "Scripts/ExceptionFlow.cs");
        Check(SemanticExceptionFlowContractValidator.IsValid(document),
            "generated exception diagnostic plan must satisfy its contract");
        Check(!SemanticExceptionFlowContractValidator.IsValid(document with
        {
            SchemaVersion = SemanticContract.CurrentSchemaVersion,
            SemanticVersion = SemanticContract.CurrentSemanticVersion,
        }), "exception plan cannot be downgraded to the executable Semantic version");
        Check(!SemanticExceptionFlowContractValidator.IsValid(document with
        {
            ExceptionFlows = null,
        }), "new Semantic identity requires an exception plan");
        Check(!SemanticExceptionFlowContractValidator.IsValid(document with
        {
            SchemaVersion = 32,
            SemanticVersion = "1.41",
        }), "the older diagnostic-only identity cannot carry projected blocks");

        SemanticExceptionFlow flow = document.ExceptionFlows!.Single();
        Check(!SemanticExceptionFlowContractValidator.IsValid(document with
        {
            ExceptionFlows = new[] { flow with { Blocks = null } },
        }), "exception block operations are required by the new contract");
        Check(!SemanticExceptionFlowContractValidator.IsValid(document with
        {
            ExceptionFlows = new[] { flow with
            {
                Catches = flow.Catches.Select(handler =>
                    handler with { RegionOrdinal = flow.Regions.Count }).ToArray(),
            } },
        }), "a handler cannot point outside the Roslyn region tree");
        Check(!SemanticExceptionFlowContractValidator.IsValid(document with
        {
            ExceptionFlows = new[] { flow with
            {
                Blocks = flow.Blocks!.Select((block, index) =>
                    index == 1 ? block with { EnclosingRegionOrdinal = flow.Regions.Count } : block).ToArray(),
            } },
        }), "out-of-range block region ownership must be rejected");
        Check(!SemanticExceptionDispatchPlanner.TryBuild(flow with
        {
            Blocks = flow.Blocks!.Select((block, index) =>
                index == 1 ? block with { EnclosingRegionOrdinal = flow.Regions.Count } : block).ToArray(),
        }, out _), "dispatch planning must reject a forged block region");
        SemanticExceptionBlock valueBlock = flow.Blocks!.First(block => block.BranchValue is not null);
        Check(!SemanticExceptionFlowContractValidator.IsValid(document with
        {
            ExceptionFlows = new[] { flow with
            {
                Blocks = flow.Blocks!.Select(block => block.Ordinal == valueBlock.Ordinal
                    ? block with { BranchValue = block.BranchValue! with
                        { Span = block.BranchValue!.Span with { Start = flow.SourceLength + 1 } } }
                    : block).ToArray(),
            } },
        }), "out-of-source branch operation must be rejected");
        SemanticExceptionRegion region = flow.Regions[1] with { ParentOrdinal = flow.Regions.Count };
        Check(!SemanticExceptionFlowContractValidator.IsValid(document with
        {
            ExceptionFlows = new[] { flow with { Regions = flow.Regions.Take(1).Append(region)
                .Concat(flow.Regions.Skip(2)).ToArray() } },
        }), "out-of-range region parent must be rejected");
        SemanticThrowSite site = flow.Throws[0] with
        {
            Span = flow.Throws[0].Span with { Start = flow.SourceLength + 1 },
        };
        Check(!SemanticExceptionFlowContractValidator.IsValid(document with
        {
            ExceptionFlows = new[] { flow with { Throws = new[] { site, flow.Throws[1] } } },
        }), "out-of-source throw span must be rejected");
        Check(!SemanticExceptionFlowContractValidator.IsValid(document with
        {
            ExceptionFlows = new[] { flow, flow },
        }), "duplicate method plans must be rejected");
        Check(!SemanticExceptionFlowContractValidator.IsValid(document with
        {
            ExceptionFlows = new[] { flow with { Branches = new[]
            {
                flow.Branches[0] with { SourceBlockOrdinal = flow.Regions[0].LastBlockOrdinal + 1 },
            } } },
        }), "branch outside the CFG block range must be rejected");
    }

    private static SemanticDocument Analyze(string source, string sourceId)
    {
        string hash = FrontendAnalyzer.Analyze(source, sourceId).Source.Sha256;
        return SemanticAnalyzer.Analyze(source, sourceId, hash);
    }

    private static bool ContainsOperation(SemanticOperation operation, string kind) =>
        operation.Kind == kind || operation.Children.Any(child => ContainsOperation(child, kind));

    private static int RegionDepth(SemanticExceptionFlow flow, int ordinal)
    {
        int depth = 0;
        while (ordinal >= 0)
        {
            ordinal = flow.Regions[ordinal].ParentOrdinal;
            ++depth;
        }
        return depth;
    }

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}
