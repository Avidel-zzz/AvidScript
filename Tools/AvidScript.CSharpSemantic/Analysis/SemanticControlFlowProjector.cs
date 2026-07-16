using System;
using System.Collections.Generic;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.FlowAnalysis;
using Microsoft.CodeAnalysis.Operations;

namespace AvidScript.CSharpSemantic;

internal sealed record SemanticControlFlowProjection(
    IReadOnlyList<SemanticControlFlowGraph> Graphs,
    IReadOnlyList<SemanticDiagnostic> Diagnostics);

internal static class SemanticControlFlowProjector
{
    public static SemanticControlFlowProjection Project(
        SemanticCompilationContext context,
        SemanticTypeRegistry typeRegistry)
    {
        List<SemanticDiagnostic> diagnostics = new();
        List<SemanticControlFlowGraph> graphs = new();
        Diagnostic? compilerError = context.Compilation.GetDiagnostics()
            .Where(diagnostic => diagnostic.Severity == DiagnosticSeverity.Error)
            .OrderBy(diagnostic => diagnostic.Location.IsInSource
                ? diagnostic.Location.SourceSpan.Start
                : int.MaxValue)
            .ThenBy(diagnostic => diagnostic.Id, StringComparer.Ordinal)
            .FirstOrDefault();
        if (compilerError is not null)
        {
            diagnostics.Add(CreateDiagnostic(
                "ASCS3004",
                "Control-flow projection requires a valid C# compilation.",
                compilerError.Location.IsInSource && compilerError.Location.SourceTree == context.SyntaxTree
                    ? SemanticSpanFactory.Create(context.SourceText, compilerError.Location.SourceSpan)
                    : SemanticSpanFactory.Empty));
            return new SemanticControlFlowProjection(Array.Empty<SemanticControlFlowGraph>(), diagnostics);
        }

        SemanticModel semanticModel = context.Compilation.GetSemanticModel(
            context.SyntaxTree,
            ignoreAccessibility: false);
        foreach (SemanticExecutableBody body in SemanticExecutableBodyResolver.Resolve(context))
        {
            SemanticSpan bodySpan = SemanticSpanFactory.Create(context.SourceText, body.Declaration.Span);
            if (body.Method.IsAsync)
            {
                diagnostics.Add(CreateDiagnostic(
                    "ASCS3002",
                    "Async control flow is not supported by the current AvidScript semantic profile.",
                    bodySpan));
                continue;
            }

            if (body.Declaration.DescendantNodesAndSelf().OfType<YieldStatementSyntax>().Any())
            {
                diagnostics.Add(CreateDiagnostic(
                    "ASCS3003",
                    "Iterator control flow is not supported by the current AvidScript semantic profile.",
                    bodySpan));
                continue;
            }

            ControlFlowGraph graph;
            try
            {
                graph = CreateGraph(body, semanticModel);
            }
            catch (ArgumentException)
            {
                diagnostics.Add(CreateInvalidGraphDiagnostic(bodySpan));
                continue;
            }
            catch (InvalidOperationException)
            {
                diagnostics.Add(CreateInvalidGraphDiagnostic(bodySpan));
                continue;
            }

            if (HasUnsupportedExceptionFlow(graph))
            {
                diagnostics.Add(CreateDiagnostic(
                    "ASCS3001",
                    "Exception control flow is not supported by the current AvidScript semantic profile.",
                    bodySpan));
                continue;
            }

            SemanticControlFlowGraph projected = ProjectGraph(
                graph,
                body.Method,
                context,
                typeRegistry);
            IReadOnlyList<string> unsupportedKinds = GetUnsupportedOperationKinds(projected);
            if (unsupportedKinds.Count > 0)
            {
                diagnostics.Add(CreateDiagnostic(
                    "ASCS3004",
                    $"The Roslyn control-flow graph contains unsupported lowered operations: {string.Join(", ", unsupportedKinds)}.",
                    bodySpan));
                continue;
            }

            graphs.Add(projected);
        }

        IReadOnlyList<SemanticDiagnostic> orderedDiagnostics = diagnostics
            .OrderBy(diagnostic => diagnostic.Span.Start)
            .ThenBy(diagnostic => diagnostic.Code, StringComparer.Ordinal)
            .ToArray();
        if (orderedDiagnostics.Count > 0)
        {
            return new SemanticControlFlowProjection(
                Array.Empty<SemanticControlFlowGraph>(),
                orderedDiagnostics);
        }

        return new SemanticControlFlowProjection(
            graphs.OrderBy(graph => graph.MethodSymbolId, StringComparer.Ordinal).ToArray(),
            orderedDiagnostics);
    }

    private static IReadOnlyList<string> GetUnsupportedOperationKinds(
        SemanticControlFlowGraph graph)
    {
        return graph.Blocks
            .SelectMany(block => block.BranchValue is null
                ? block.Operations
                : block.Operations.Append(block.BranchValue))
            .SelectMany(EnumerateOperation)
            .Where(operation => !operation.IsSupported)
            .Select(operation => operation.Kind)
            .Distinct(StringComparer.Ordinal)
            .OrderBy(kind => kind, StringComparer.Ordinal)
            .ToArray();
    }

    private static IEnumerable<SemanticOperation> EnumerateOperation(SemanticOperation operation)
    {
        yield return operation;
        foreach (SemanticOperation child in operation.Children)
        {
            foreach (SemanticOperation descendant in EnumerateOperation(child))
            {
                yield return descendant;
            }
        }
    }

    private static ControlFlowGraph CreateGraph(
        SemanticExecutableBody body,
        SemanticModel semanticModel)
    {
        return body.Operation switch
        {
            IMethodBodyOperation methodBody => ControlFlowGraph.Create(methodBody),
            IConstructorBodyOperation constructorBody => ControlFlowGraph.Create(constructorBody),
            IBlockOperation block when block.Parent is null => ControlFlowGraph.Create(block),
            _ => CreateSyntaxGraph(body.Declaration, semanticModel),
        };
    }

    private static ControlFlowGraph CreateSyntaxGraph(
        SyntaxNode declaration,
        SemanticModel semanticModel)
    {
        SyntaxNode graphRoot = declaration switch
        {
            PropertyDeclarationSyntax property => property.ExpressionBody!.Expression,
            IndexerDeclarationSyntax indexer => indexer.ExpressionBody!.Expression,
            _ => declaration,
        };
        return ControlFlowGraph.Create(graphRoot, semanticModel) ??
            throw new InvalidOperationException("Roslyn did not create a control-flow graph.");
    }

    private static bool HasUnsupportedExceptionFlow(ControlFlowGraph graph)
    {
        bool hasUnsupportedRegion = EnumerateRegions(graph.Root).Any(region =>
            region.Kind is not ControlFlowRegionKind.Root and not ControlFlowRegionKind.LocalLifetime);
        bool hasUnsupportedBranch = graph.Blocks
            .SelectMany(GetAllBranches)
            .Any(branch => branch.Semantics is
                ControlFlowBranchSemantics.StructuredExceptionHandling or
                ControlFlowBranchSemantics.Throw or
                ControlFlowBranchSemantics.Rethrow or
                ControlFlowBranchSemantics.Error);
        return hasUnsupportedRegion || hasUnsupportedBranch;
    }

    private static IEnumerable<ControlFlowRegion> EnumerateRegions(ControlFlowRegion region)
    {
        yield return region;
        foreach (ControlFlowRegion nested in region.NestedRegions)
        {
            foreach (ControlFlowRegion descendant in EnumerateRegions(nested))
            {
                yield return descendant;
            }
        }
    }

    private static SemanticControlFlowGraph ProjectGraph(
        ControlFlowGraph graph,
        IMethodSymbol method,
        SemanticCompilationContext context,
        SemanticTypeRegistry typeRegistry)
    {
        SemanticCaptureRegistry captureRegistry = new();
        IReadOnlyList<SemanticBasicBlock> blocks = graph.Blocks
            .OrderBy(block => block.Ordinal)
            .Select(block => ProjectBlock(block, context, typeRegistry, captureRegistry))
            .ToArray();
        return new SemanticControlFlowGraph(
            SemanticSymbolProjector.GetSymbolId(method),
            blocks.Single(block => block.Kind == "entry").Ordinal,
            blocks.Single(block => block.Kind == "exit").Ordinal,
            blocks);
    }

    private static SemanticBasicBlock ProjectBlock(
        BasicBlock block,
        SemanticCompilationContext context,
        SemanticTypeRegistry typeRegistry,
        SemanticCaptureRegistry captureRegistry)
    {
        return new SemanticBasicBlock(
            block.Ordinal,
            MapBlockKind(block.Kind),
            block.IsReachable,
            MapConditionKind(block.ConditionKind),
            block.Operations
                .Select(operation => SemanticOperationProjector.ProjectControlFlowOperation(
                    operation,
                    context,
                    typeRegistry,
                    captureRegistry))
                .ToArray(),
            block.BranchValue is { } branchValue
                ? SemanticOperationProjector.ProjectControlFlowOperation(
                    branchValue,
                    context,
                    typeRegistry,
                    captureRegistry)
                : null,
            block.Predecessors
                .Select(ProjectEdge)
                .OrderBy(edge => edge.SourceBlockOrdinal)
                .ThenBy(edge => edge.DestinationBlockOrdinal)
                .ThenBy(edge => edge.Kind, StringComparer.Ordinal)
                .ThenBy(edge => edge.Semantics, StringComparer.Ordinal)
                .ToArray(),
            GetBranches(block)
                .Select(ProjectEdge)
                .OrderBy(edge => edge.SourceBlockOrdinal)
                .ThenBy(edge => edge.DestinationBlockOrdinal)
                .ThenBy(edge => edge.Kind, StringComparer.Ordinal)
                .ThenBy(edge => edge.Semantics, StringComparer.Ordinal)
                .ToArray());
    }

    private static IEnumerable<ControlFlowBranch> GetAllBranches(BasicBlock block)
    {
        if (block.FallThroughSuccessor is { } fallThrough)
        {
            yield return fallThrough;
        }

        if (block.ConditionalSuccessor is { } conditional)
        {
            yield return conditional;
        }
    }

    private static IEnumerable<ControlFlowBranch> GetBranches(BasicBlock block)
    {
        foreach (ControlFlowBranch branch in GetAllBranches(block))
        {
            if (branch.Destination is not null)
            {
                yield return branch;
            }
        }
    }

    private static SemanticControlFlowEdge ProjectEdge(ControlFlowBranch branch)
    {
        return new SemanticControlFlowEdge(
            branch.Source.Ordinal,
            branch.Destination!.Ordinal,
            branch.IsConditionalSuccessor ? "conditional" : "fallthrough",
            MapBranchSemantics(branch.Semantics));
    }

    private static string MapBlockKind(BasicBlockKind kind)
    {
        return kind switch
        {
            BasicBlockKind.Entry => "entry",
            BasicBlockKind.Exit => "exit",
            BasicBlockKind.Block => "block",
            _ => "roslyn:" + kind.ToString().ToLowerInvariant(),
        };
    }

    private static string MapConditionKind(ControlFlowConditionKind kind)
    {
        return kind switch
        {
            ControlFlowConditionKind.None => "none",
            ControlFlowConditionKind.WhenFalse => "when_false",
            ControlFlowConditionKind.WhenTrue => "when_true",
            _ => "roslyn:" + kind.ToString().ToLowerInvariant(),
        };
    }

    private static string MapBranchSemantics(ControlFlowBranchSemantics semantics)
    {
        return semantics switch
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
    }

    private static SemanticDiagnostic CreateInvalidGraphDiagnostic(SemanticSpan span)
    {
        return CreateDiagnostic(
            "ASCS3004",
            "Roslyn could not create a valid control-flow graph for this executable body.",
            span);
    }

    private static SemanticDiagnostic CreateDiagnostic(string code, string message, SemanticSpan span)
    {
        return new SemanticDiagnostic(code, "error", message, span);
    }
}
