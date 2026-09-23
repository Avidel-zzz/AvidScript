using System;
using System.Collections.Generic;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace AvidScript.CSharpSemantic;

// Roslyn's general CFG expresses even array foreach through an enumerator and
// a finally region. This projector reuses the controlled async array iteration
// plan, then publishes ordinary, synchronous basic blocks for Guest lowering.
internal static class SemanticArrayForEachControlFlowProjector
{
    public static bool IsCandidate(
        SemanticExecutableBody body,
        SemanticModel semanticModel)
    {
        if (GetBody(body.Declaration) is not { } block)
        {
            return false;
        }

        ForEachStatementSyntax[] loops = block.DescendantNodes()
            .OfType<ForEachStatementSyntax>()
            .Where(loop => IsOwnedBy(loop, body.Declaration))
            .ToArray();
        if (loops.Length == 0 || block.DescendantNodes()
            .OfType<ForEachVariableStatementSyntax>()
            .Any(loop => IsOwnedBy(loop, body.Declaration)))
        {
            return false;
        }

        foreach (ForEachStatementSyntax loop in loops)
        {
            if (!loop.AwaitKeyword.IsKind(SyntaxKind.None)
                || loop.Type is RefTypeSyntax
                || semanticModel.GetTypeInfo(loop.Expression).Type is not IArrayTypeSymbol { Rank: 1 } arrayType
                || semanticModel.GetDeclaredSymbol(loop) is not ILocalSymbol item
                || !SymbolEqualityComparer.Default.Equals(item.Type, arrayType.ElementType)
                || IsIterationVariableCaptured(loop, item, semanticModel))
            {
                return false;
            }
        }
        return true;
    }

    public static bool TryProject(
        SemanticCompilationContext context,
        SemanticExecutableBody body,
        SemanticModel semanticModel,
        SemanticTypeRegistry typeRegistry,
        ICollection<SemanticDiagnostic> diagnostics,
        out SemanticControlFlowGraph? graph,
        out IReadOnlyList<SemanticSymbol> compilerLocalSymbols)
    {
        graph = null;
        compilerLocalSymbols = Array.Empty<SemanticSymbol>();
        BlockSyntax block = GetBody(body.Declaration)!;
        string methodSymbolId = SemanticSymbolProjector.GetSymbolId(body.Method);
        typeRegistry.Register(context.Compilation.GetSpecialType(SpecialType.System_Boolean));
        int nextCallbackId = 0;
        SemanticCompilationContext unitContext = context with { PrimaryUnit = body.Unit };
        if (!SemanticAsyncControlFlowProjector.TryProject(
            unitContext,
            semanticModel,
            block,
            methodSymbolId,
            typeRegistry,
            diagnostics,
            ref nextCallbackId,
            out SemanticAsyncControlFlowProjection? flow,
            allowValueReturns: true))
        {
            return false;
        }

        if (nextCallbackId != 0 || flow!.Segments.Any(segment => segment.AwaitSite is not null))
        {
            diagnostics.Add(Error(body, "A synchronous array foreach cannot contain an await site."));
            return false;
        }

        int exitOrdinal = flow.Segments.Count;
        List<SemanticControlFlowEdge> edges = new();
        List<SemanticBasicBlock> blocks = new(flow.Segments.Count + 1);
        foreach (SemanticAsyncSegment segment in flow.Segments)
        {
            List<SemanticOperation> operations = new();
            foreach (SemanticAsyncStatement statement in segment.Statements)
            {
                if (!TryAddStatement(statement, operations))
                {
                    diagnostics.Add(Error(body,
                        $"Array foreach contains a statement that cannot be lowered as synchronous control flow: {statement.Operation.Kind}."));
                    return false;
                }
            }

            SemanticAsyncControlTransfer? transfer = segment.Transfer;
            if (transfer is null)
            {
                diagnostics.Add(Error(body, "Array foreach has no control-flow transfer."));
                return false;
            }
            SemanticOperation? branchValue = null;
            string conditionKind = "none";
            switch (transfer.Kind)
            {
                case SemanticAsyncMethod.GotoTransferKind when transfer.PrimaryTarget >= 0:
                    edges.Add(new(segment.Ordinal, transfer.PrimaryTarget, "fallthrough", "regular"));
                    break;
                case SemanticAsyncMethod.BranchTransferKind when transfer.Condition is not null
                    && transfer.PrimaryTarget >= 0 && transfer.SecondaryTarget >= 0:
                    branchValue = transfer.Condition;
                    conditionKind = "when_true";
                    edges.Add(new(segment.Ordinal, transfer.PrimaryTarget, "conditional", "regular"));
                    edges.Add(new(segment.Ordinal, transfer.SecondaryTarget, "fallthrough", "regular"));
                    break;
                case SemanticAsyncMethod.ReturnTransferKind:
                    branchValue = transfer.Condition;
                    edges.Add(new(segment.Ordinal, exitOrdinal, "fallthrough", "return"));
                    break;
                default:
                    diagnostics.Add(Error(body, $"Array foreach has unsupported transfer '{transfer.Kind}'."));
                    return false;
            }

            blocks.Add(new SemanticBasicBlock(
                segment.Ordinal,
                segment.Ordinal == flow.EntrySegmentOrdinal ? "entry" : "block",
                true,
                conditionKind,
                operations,
                branchValue,
                Array.Empty<SemanticControlFlowEdge>(),
                Array.Empty<SemanticControlFlowEdge>()));
        }
        blocks.Add(new SemanticBasicBlock(
            exitOrdinal, "exit", true, "none",
            Array.Empty<SemanticOperation>(), null,
            Array.Empty<SemanticControlFlowEdge>(),
            Array.Empty<SemanticControlFlowEdge>()));

        SemanticControlFlowEdge[] orderedEdges = edges
            .OrderBy(edge => edge.SourceBlockOrdinal)
            .ThenBy(edge => edge.DestinationBlockOrdinal)
            .ThenBy(edge => edge.Kind, StringComparer.Ordinal)
            .ToArray();
        graph = new SemanticControlFlowGraph(
            methodSymbolId,
            flow.EntrySegmentOrdinal,
            exitOrdinal,
            blocks.Select(item => item with
            {
                Predecessors = orderedEdges.Where(edge => edge.DestinationBlockOrdinal == item.Ordinal).ToArray(),
                Successors = orderedEdges.Where(edge => edge.SourceBlockOrdinal == item.Ordinal).ToArray(),
            }).ToArray());
        compilerLocalSymbols = flow.CompilerLocals.Select(local => new SemanticSymbol(
            local.SymbolId,
            "local",
            local.Name,
            methodSymbolId,
            local.TypeId,
            local.Name + ":" + local.TypeId,
            false,
            "not_applicable",
            local.Span)
        {
            IsExecutableReferenceSource = !body.Unit.IsPrimary,
        }).ToArray();
        return true;
    }

    private static bool TryAddStatement(
        SemanticAsyncStatement statement,
        ICollection<SemanticOperation> operations)
    {
        if (statement.TargetSymbolId is { } target)
        {
            operations.Add(Assign(target, statement.Operation));
            return true;
        }
        SemanticOperation operation = statement.Operation;
        if (operation.Kind == SemanticAsyncMethod.BlockOperationKind)
        {
            foreach (SemanticOperation child in operation.Children)
            {
                if (!TryAddStatement(new SemanticAsyncStatement(child, null), operations)) return false;
            }
            return true;
        }
        if (operation.Kind == SemanticAsyncMethod.LocalDeclarationOperationKind)
        {
            if (operation.SymbolId is null || operation.Children.Count > 1) return false;
            if (operation.Children.Count == 1)
                operations.Add(Assign(operation.SymbolId, operation.Children[0]));
            return true;
        }
        if (operation.Kind.StartsWith("async_", StringComparison.Ordinal)) return false;
        operations.Add(operation);
        return true;
    }

    private static SemanticOperation Assign(string targetSymbolId, SemanticOperation value)
    {
        SemanticOperation target = new(
            "local_reference", true, null, false, false, false, false,
            value.TypeId, targetSymbolId, Array.Empty<string>(), null, null,
            null, null, null, value.Span, Array.Empty<SemanticOperation>());
        return new SemanticOperation(
            "assignment", true, null, false, false, false, false,
            value.TypeId, null, Array.Empty<string>(), null, null,
            null, null, null, value.Span, new[] { target, value });
    }

    private static BlockSyntax? GetBody(SyntaxNode declaration) => declaration switch
    {
        BaseMethodDeclarationSyntax method => method.Body,
        AccessorDeclarationSyntax accessor => accessor.Body,
        LocalFunctionStatementSyntax local => local.Body,
        AnonymousFunctionExpressionSyntax lambda => lambda.Body as BlockSyntax,
        _ => null,
    };

    private static bool IsOwnedBy(SyntaxNode syntax, SyntaxNode declaration) =>
        syntax.Ancestors().FirstOrDefault(SemanticExecutableBodyResolver.IsExecutableDeclaration) == declaration;

    private static bool IsIterationVariableCaptured(
        ForEachStatementSyntax loop,
        ILocalSymbol item,
        SemanticModel semanticModel) =>
        loop.Statement.DescendantNodes()
            .OfType<IdentifierNameSyntax>()
            .Any(identifier => identifier.Ancestors().Any(node =>
                    node is AnonymousFunctionExpressionSyntax or LocalFunctionStatementSyntax)
                && SymbolEqualityComparer.Default.Equals(
                    semanticModel.GetSymbolInfo(identifier).Symbol,
                    item));

    private static SemanticDiagnostic Error(SemanticExecutableBody body, string message) =>
        new("ASCS3004", "error", message,
            SemanticSpanFactory.Create(body.Unit.SourceText, body.Declaration.Span));
}
