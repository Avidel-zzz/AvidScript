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
        NestedCallableOwnsItsThrowSite();
        OrdinaryArtifactsKeepTheirOriginalShape();
        ContractValidatorRejectsDowngradeAndCorruption();
        return 6;
    }

    private static void NestedThrowCatchRethrowKeepsRoslynRegions()
    {
        SemanticDocument document = Analyze(NestedSource, "Scripts/ExceptionFlow.cs");
        Check(!document.Succeeded && document.SchemaVersion == 32
            && document.SemanticVersion == "1.41", "exception plans require a separate failed Semantic identity");
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
            && !flow.Catches[0].HasFilter,
            "typed catch must retain type matching and filter status");
        Check(flow.Regions.Any(region => region.Kind == "catch")
            && flow.Regions.Count(region => region.Kind == "finally") >= 2,
            "nested catch and cleanup regions must survive projection");
        Check(flow.Branches.Any(branch => branch.Semantics is "throw" or "rethrow")
            && flow.Branches.Any(branch => branch.FinallyRegionOrdinals.Count > 0),
            "Roslyn exception and cleanup branch effects must survive projection");
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
        SemanticDiagnostic diagnostic = document.Diagnostics.Single(item => item.Code == "ASCS3005");
        Check(source.Substring(diagnostic.Span.Start, diagnostic.Span.Length) == "when (value > 0)",
            "unsupported filter diagnostic must point at the filter");
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

        SemanticExceptionFlow flow = document.ExceptionFlows!.Single();
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

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}
