using System;
using System.Collections.Generic;
using System.Linq;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Text;

namespace AvidScript.CSharpSemantic;

// Pairs draft-owned segment memberships with Roslyn region identities. Segment
// membership never comes from source-span containment: copied finally blocks
// have the same span but different draft identities and exit targets.
internal static class SemanticAsyncExceptionRegionBinder
{
    public static bool TryBind(
        SemanticCompilationContext context,
        MethodDeclarationSyntax declaration,
        SemanticExceptionFlow flow,
        SemanticAsyncControlFlowProjection projection,
        out IReadOnlyList<SemanticAsyncExceptionPreviewRegion> bound,
        out IReadOnlyList<SemanticAsyncExceptionScope> scopes)
    {
        bound = Array.Empty<SemanticAsyncExceptionPreviewRegion>();
        scopes = Array.Empty<SemanticAsyncExceptionScope>();
        if (flow.Regions is null || flow.Catches is null
            || projection.PreviewRegions.Count == 0) return false;

        TryStatementSyntax[] sourceTries = declaration.Body!.DescendantNodes()
            .OfType<TryStatementSyntax>()
            .Where(node => OwnedBy(node, declaration))
            .OrderBy(node => node.SpanStart).ToArray();
        FinallyClauseSyntax[] sourceFinalies = sourceTries
            .Select(node => node.Finally).OfType<FinallyClauseSyntax>()
            .OrderBy(node => node.SpanStart).ToArray();
        SemanticExceptionRegion[] roslynFinalies = flow.Regions
            .Where(region => region.Kind == "finally")
            .OrderBy(region => region.Ordinal).ToArray();
        // Roslyn can synthesize cleanup regions for other constructs. Until
        // they have a source owner here, no executable preview is published.
        if (sourceFinalies.Length != roslynFinalies.Length) return false;
        Dictionary<TextSpan, int> finallyOrdinals = sourceFinalies
            .Select((source, index) => (source.Span, roslynFinalies[index].Ordinal))
            .ToDictionary(item => item.Span, item => item.Ordinal);

        List<SemanticAsyncExceptionPreviewRegion> regions = new();
        Dictionary<TryStatementSyntax, (int Enclosing, int Protected,
            int Finally, int[] Catches)> bindings = new();
        foreach (TryStatementSyntax source in sourceTries)
        {
            SemanticAsyncPreviewRegionDraft[] drafts = projection.PreviewRegions
                .Where(region => region.TrySpan == source.Span).ToArray();
            if (drafts.Length == 0) continue;
            if (drafts.Length != 1 + source.Catches.Count
                + (source.Finally is null ? 0 : 1)
                || drafts.Count(region => region.Kind == "try"
                    && region.PartSpan == source.Block.Span) != 1)
                return false;

            SemanticCatchHandler[] catches = new SemanticCatchHandler[source.Catches.Count];
            for (int index = 0; index < catches.Length; ++index)
            {
                CatchClauseSyntax clause = source.Catches[index];
                SemanticCatchHandler? handler = flow.Catches.SingleOrDefault(item =>
                    SameSpan(item.Span, clause.Span));
                if (handler is null
                    || drafts.Count(region => region.Kind == "catch"
                        && region.PartSpan == clause.Span) != 1)
                    return false;
                catches[index] = handler;
            }

            int finallyOrdinal = -1;
            if (source.Finally is { } finallyClause)
            {
                if (!finallyOrdinals.TryGetValue(finallyClause.Span,
                        out finallyOrdinal)
                    || drafts.Count(region => region.Kind == "finally"
                        && region.PartSpan == finallyClause.Span) != 1)
                    return false;
                int parent = flow.Regions[finallyOrdinal].ParentOrdinal;
                if (!IsKind(flow.Regions, parent, "try_and_finally")) return false;
            }

            int protectedOrdinal;
            int enclosingOrdinal;
            if (catches.Length > 0)
            {
                int group = flow.Regions[catches[0].RegionOrdinal].ParentOrdinal;
                if (!IsKind(flow.Regions, group, "try_and_catch")
                    || catches.Any(catchHandler =>
                        flow.Regions[catchHandler.RegionOrdinal].ParentOrdinal != group)
                    || !TryDirectChild(flow.Regions, group, "try",
                        out protectedOrdinal)) return false;
                if (finallyOrdinal >= 0
                    && !IsDescendantOf(flow.Regions, group,
                        flow.Regions[finallyOrdinal].ParentOrdinal)) return false;
                enclosingOrdinal = finallyOrdinal >= 0
                    ? flow.Regions[finallyOrdinal].ParentOrdinal : group;
            }
            else
            {
                if (finallyOrdinal < 0
                    || !TryDirectChild(flow.Regions,
                        flow.Regions[finallyOrdinal].ParentOrdinal, "try",
                        out protectedOrdinal)) return false;
                enclosingOrdinal = flow.Regions[finallyOrdinal].ParentOrdinal;
            }
            bindings.Add(source, (enclosingOrdinal, protectedOrdinal,
                finallyOrdinal, catches.Select(handler => handler.RegionOrdinal).ToArray()));

            foreach (SemanticAsyncPreviewRegionDraft draft in drafts)
            {
                int ordinal = draft.Kind switch
                {
                    "try" => protectedOrdinal,
                    "finally" => finallyOrdinal,
                    "catch" => catches.Single(handler =>
                        SameSpan(handler.Span, draft.PartSpan)).RegionOrdinal,
                    _ => -1,
                };
                if (ordinal < 0 || ordinal >= flow.Regions.Count
                    || flow.Regions[ordinal].Kind != draft.Kind
                    || draft.Segments.Any(segment =>
                        segment < 0 || segment >= projection.Segments.Count)
                    || !draft.Segments.SequenceEqual(
                        draft.Segments.Distinct().Order())) return false;
                regions.Add(new SemanticAsyncExceptionPreviewRegion(
                    draft.Kind,
                    ordinal,
                    SemanticSpanFactory.Create(context.PrimaryUnit.SourceText,
                        draft.PartSpan),
                    draft.Segments));
            }
        }
        if (regions.Count != projection.PreviewRegions.Count
            || regions.Select(region => region.RoslynRegionOrdinal).Distinct().Count()
                != regions.Count
            || projection.Segments.Any(segment => segment.Transfer is
                { Kind: SemanticAsyncMethod.AwaitTransferKind,
                    SecondaryTarget: >= 0 } transfer
                && (transfer.CancellationTarget is not >= 0
                    || projection.ExceptionScopes.Count == 0
                        && transfer.CancellationTarget == transfer.SecondaryTarget
                    || !regions.Any(region => region.Kind == "try"
                        && region.Segments.Contains(segment.Ordinal))))) return false;
        foreach (var (source, binding) in bindings)
        {
            TryStatementSyntax? parent = source.Ancestors()
                .OfType<TryStatementSyntax>()
                .FirstOrDefault(node => bindings.ContainsKey(node));
            if (parent is null) continue;
            var owner = bindings[parent];
            int containingRegion = parent.Block.Span.Contains(source.Span)
                ? owner.Protected
                : parent.Finally?.Span.Contains(source.Span) == true
                    ? owner.Finally
                    : parent.Catches.Select((clause, index) => (clause, index))
                        .Where(item => item.clause.Span.Contains(source.Span))
                        .Select(item => owner.Catches[item.index])
                        .DefaultIfEmpty(-1).First();
            if (containingRegion < 0
                || !IsDescendantOf(flow.Regions, binding.Enclosing,
                    containingRegion)) return false;
        }
        bound = regions.OrderBy(region => region.SourceSpan.Start)
            .ThenBy(region => region.Kind, StringComparer.Ordinal)
            .ToArray();
        List<SemanticAsyncExceptionScope> exceptionScopes = new();
        foreach (SemanticAsyncExceptionScopeDraft draft in projection.ExceptionScopes)
        {
            TryStatementSyntax? source = bindings.Keys.SingleOrDefault(item => item.Span == draft.TrySpan);
            if (source is null) return false;
            var binding = bindings[source];
            TryStatementSyntax? parent = source.Ancestors().OfType<TryStatementSyntax>()
                .FirstOrDefault(bindings.ContainsKey);
            if (parent is not null && !parent.Block.Span.Contains(source.Span)) return false;
            exceptionScopes.Add(new(binding.Protected, binding.Catches,
                binding.Finally < 0 ? null : binding.Finally,
                parent is null ? null : bindings[parent].Protected,
                draft.DispatchTarget, draft.UnwindTarget));
        }
        scopes = exceptionScopes.OrderBy(scope => scope.ProtectedRegionOrdinal).ToArray();
        return true;
    }

    private static bool SameSpan(SemanticSpan source, TextSpan syntax) =>
        source.Start == syntax.Start && source.Length == syntax.Length;

    private static bool OwnedBy(TryStatementSyntax source,
        MethodDeclarationSyntax declaration) =>
        source.Ancestors().FirstOrDefault(
            SemanticExecutableBodyResolver.IsExecutableDeclaration) == declaration;

    private static bool IsKind(IReadOnlyList<SemanticExceptionRegion> regions,
        int ordinal, string kind) =>
        ordinal >= 0 && ordinal < regions.Count && regions[ordinal].Kind == kind;

    private static bool TryDirectChild(
        IReadOnlyList<SemanticExceptionRegion> regions,
        int parent,
        string kind,
        out int child)
    {
        int[] matching = regions.Where(region => region.ParentOrdinal == parent
            && region.Kind == kind).Select(region => region.Ordinal).ToArray();
        child = matching.Length == 1 ? matching[0] : -1;
        return child >= 0;
    }

    private static bool IsDescendantOf(
        IReadOnlyList<SemanticExceptionRegion> regions,
        int child,
        int ancestor)
    {
        while (child >= 0 && child < regions.Count)
        {
            if (child == ancestor) return true;
            child = regions[child].ParentOrdinal;
        }
        return false;
    }
}
