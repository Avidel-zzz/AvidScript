using System;
using System.Collections.Generic;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.FlowAnalysis;

namespace AvidScript.CSharpSemantic;

internal static class SemanticExceptionFlowProjector
{
    private const int MaximumRegions = 512;
    private const int MaximumBranches = 4096;
    private const int MaximumSites = 256;

    public static bool IsCandidate(SemanticExecutableBody body) =>
        body.Declaration.DescendantNodes().Any(node =>
            node is CatchClauseSyntax or ThrowStatementSyntax or ThrowExpressionSyntax
            && IsOwnedBy(node, body.Declaration));

    public static bool IsAsyncRegionCandidate(SemanticExecutableBody body) =>
        IsCandidate(body) || body.Declaration.DescendantNodes()
            .OfType<FinallyClauseSyntax>()
            .Any(node => IsOwnedBy(node, body.Declaration));

    public static SemanticExceptionFlow? Project(
        SemanticExecutableBody body,
        SemanticModel semanticModel,
        ControlFlowGraph graph,
        SemanticTypeRegistry typeRegistry,
        ICollection<SemanticDiagnostic> diagnostics)
    {
        List<ControlFlowRegion> roslynRegions = new();
        Dictionary<ControlFlowRegion, int> ordinals = new();
        Stack<ControlFlowRegion> pending = new();
        pending.Push(graph.Root);
        while (pending.TryPop(out ControlFlowRegion? region))
        {
            if (roslynRegions.Count > MaximumRegions) break;
            ordinals.Add(region, roslynRegions.Count);
            roslynRegions.Add(region);
            foreach (ControlFlowRegion nested in region.NestedRegions.Reverse()) pending.Push(nested);
        }
        CatchClauseSyntax[] catches = body.Declaration.DescendantNodes()
            .OfType<CatchClauseSyntax>()
            .Where(catchClause => IsOwnedBy(catchClause, body.Declaration))
            .OrderBy(catchClause => catchClause.SpanStart)
            .ToArray();
        SyntaxNode[] throws = body.Declaration.DescendantNodes()
            .Where(node => node is ThrowStatementSyntax or ThrowExpressionSyntax
                && IsOwnedBy(node, body.Declaration))
            .OrderBy(node => node.SpanStart)
            .ToArray();
        if (roslynRegions.Count > MaximumRegions || graph.Blocks.Length > MaximumBranches / 2
            || catches.Length > MaximumSites || throws.Length > MaximumSites)
        {
            diagnostics.Add(new SemanticDiagnostic("ASCS3005", "error",
                "Exception flow exceeds the bounded Semantic projection budget.",
                SemanticSpanFactory.Create(body.Unit.SourceText, body.Declaration.Span)));
            return null;
        }

        SemanticExceptionRegion[] regions = roslynRegions.Select(region =>
            new SemanticExceptionRegion(
                ordinals[region],
                region.EnclosingRegion is { } parent && ordinals.TryGetValue(parent, out int parentOrdinal)
                    ? parentOrdinal : -1,
                MapRegionKind(region.Kind),
                region.FirstBlockOrdinal,
                region.LastBlockOrdinal,
                region.ExceptionType is { } exceptionType
                    ? typeRegistry.Register(exceptionType) : null)).ToArray();
        List<SemanticExceptionBranch> branches = new();
        foreach (BasicBlock block in graph.Blocks)
        {
            if (block.FallThroughSuccessor is { } fallthrough)
                branches.Add(ProjectBranch(fallthrough, "fallthrough", ordinals));
            if (block.ConditionalSuccessor is { } conditional)
                branches.Add(ProjectBranch(conditional, "conditional", ordinals));
        }

        SemanticThrowSite[] throwSites = throws.Select(node =>
        {
            SyntaxNode? value = node switch
            {
                ThrowStatementSyntax statement => statement.Expression,
                ThrowExpressionSyntax expression => expression.Expression,
                _ => null,
            };
            ITypeSymbol? type = value is null ? null : semanticModel.GetTypeInfo(value).Type;
            return new SemanticThrowSite(
                value is null ? "rethrow" : "throw",
                type is null ? null : typeRegistry.Register(type),
                SemanticSpanFactory.Create(body.Unit.SourceText, node.Span));
        }).ToArray();
        ControlFlowRegion[] catchRegions = roslynRegions
            .Where(region => region.Kind == ControlFlowRegionKind.Catch)
            .ToArray();
        if (catchRegions.Length != catches.Length)
        {
            diagnostics.Add(new SemanticDiagnostic("ASCS3005", "error",
                "Roslyn catch regions do not match the source handlers.",
                SemanticSpanFactory.Create(body.Unit.SourceText, body.Declaration.Span)));
            return null;
        }
        SemanticCatchHandler[] handlers = catches.Select((catchClause, ordinal) =>
        {
            ITypeSymbol? type = catchClause.Declaration is { } declaration
                ? semanticModel.GetTypeInfo(declaration.Type).Type : null;
            ISymbol? variable = catchClause.Declaration is { } catchDeclaration
                ? semanticModel.GetDeclaredSymbol(catchDeclaration) : null;
            if (catchClause.Filter is { } filter)
            {
                diagnostics.Add(new SemanticDiagnostic("ASCS3005", "error",
                    "Catch filters are not supported by the current exception flow contract.",
                    SemanticSpanFactory.Create(body.Unit.SourceText, filter.Span)));
            }
            return new SemanticCatchHandler(
                ordinal,
                type is null ? null : typeRegistry.Register(type),
                variable is null ? null : SemanticSymbolProjector.GetSymbolId(variable),
                catchClause.Filter is not null,
                SemanticSpanFactory.Create(body.Unit.SourceText, catchClause.Span),
                ordinals[catchRegions[ordinal]]);
        }).ToArray();
        SemanticCatchHandler? mismatchedHandler = handlers.FirstOrDefault(handler =>
            handler.ExceptionTypeId != regions[handler.RegionOrdinal].ExceptionTypeId
            && !(handler.ExceptionTypeId is null
                && regions[handler.RegionOrdinal].ExceptionTypeId == "type:object"));
        if (mismatchedHandler is not null)
        {
            diagnostics.Add(new SemanticDiagnostic("ASCS3005", "error",
                $"Roslyn catch region type '{regions[mismatchedHandler.RegionOrdinal].ExceptionTypeId}' " +
                    $"does not match source handler '{mismatchedHandler.ExceptionTypeId}'.",
                SemanticSpanFactory.Create(body.Unit.SourceText, body.Declaration.Span)));
            return null;
        }
        SemanticCaptureRegistry captureRegistry = new();
        SemanticExceptionBlock[] blocks = graph.Blocks
            .OrderBy(block => block.Ordinal)
            .Select(block => new SemanticExceptionBlock(
                block.Ordinal,
                block.Kind switch
                {
                    BasicBlockKind.Entry => "entry",
                    BasicBlockKind.Exit => "exit",
                    _ => "block",
                },
                block.IsReachable,
                block.ConditionKind switch
                {
                    ControlFlowConditionKind.WhenTrue => "when_true",
                    ControlFlowConditionKind.WhenFalse => "when_false",
                    _ => "none",
                },
                roslynRegions
                    .Where(region => region.FirstBlockOrdinal <= block.Ordinal
                        && region.LastBlockOrdinal >= block.Ordinal)
                    .Select(region => ordinals[region])
                    .Last(),
                block.Operations.Select(operation =>
                    SemanticOperationProjector.ProjectControlFlowOperation(
                        operation, body.Unit, typeRegistry, captureRegistry)).ToArray(),
                block.BranchValue is { } branchValue
                    ? SemanticOperationProjector.ProjectControlFlowOperation(
                        branchValue, body.Unit, typeRegistry, captureRegistry)
                    : null))
            .ToArray();
        return new SemanticExceptionFlow(
            SemanticSymbolProjector.GetSymbolId(body.Method),
            body.Unit.SyntaxTree.FilePath,
            body.Unit.SourceText.Length,
            regions, branches, throwSites, handlers, blocks);

    }

    private static SemanticExceptionBranch ProjectBranch(
        ControlFlowBranch branch,
        string kind,
        IReadOnlyDictionary<ControlFlowRegion, int> ordinals) =>
        new(branch.Source.Ordinal, branch.Destination?.Ordinal ?? -1,
            kind, MapBranchSemantics(branch.Semantics),
            LocalRegions(branch.LeavingRegions, ordinals),
            LocalRegions(branch.EnteringRegions, ordinals),
            LocalRegions(branch.FinallyRegions, ordinals));

    private static int[] LocalRegions(
        IEnumerable<ControlFlowRegion> regions,
        IReadOnlyDictionary<ControlFlowRegion, int> ordinals) =>
        regions.Where(ordinals.ContainsKey).Select(region => ordinals[region]).ToArray();

    private static string MapRegionKind(ControlFlowRegionKind kind) => kind switch
    {
        ControlFlowRegionKind.Root => "root",
        ControlFlowRegionKind.LocalLifetime => "local_lifetime",
        ControlFlowRegionKind.Try => "try",
        ControlFlowRegionKind.Catch => "catch",
        ControlFlowRegionKind.Finally => "finally",
        ControlFlowRegionKind.TryAndCatch => "try_and_catch",
        ControlFlowRegionKind.TryAndFinally => "try_and_finally",
        ControlFlowRegionKind.Filter => "filter",
        ControlFlowRegionKind.FilterAndHandler => "filter_and_handler",
        ControlFlowRegionKind.StaticLocalInitializer => "static_local_initializer",
        ControlFlowRegionKind.ErroneousBody => "erroneous_body",
        _ => "roslyn:" + kind.ToString().ToLowerInvariant(),
    };

    private static string MapBranchSemantics(ControlFlowBranchSemantics semantics) => semantics switch
    {
        ControlFlowBranchSemantics.None => "none",
        ControlFlowBranchSemantics.Regular => "regular",
        ControlFlowBranchSemantics.Return => "return",
        ControlFlowBranchSemantics.ProgramTermination => "program_termination",
        ControlFlowBranchSemantics.StructuredExceptionHandling => "structured_exception_handling",
        ControlFlowBranchSemantics.Throw => "throw",
        ControlFlowBranchSemantics.Rethrow => "rethrow",
        ControlFlowBranchSemantics.Error => "error",
        _ => "roslyn:" + semantics.ToString().ToLowerInvariant(),
    };

    private static bool IsOwnedBy(SyntaxNode syntax, SyntaxNode declaration) =>
        syntax.Ancestors().FirstOrDefault(SemanticExecutableBodyResolver.IsExecutableDeclaration)
            == declaration;
}
