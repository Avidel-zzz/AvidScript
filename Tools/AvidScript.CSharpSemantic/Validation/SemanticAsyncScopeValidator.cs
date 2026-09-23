using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.CSharpSemantic;

public static class SemanticAsyncScopeValidator
{
    public static bool IsValid(SemanticDocument document)
    {
        if (document.AsyncMethods is null || document.ClosureEnvironments is null) return false;
        foreach (SemanticAsyncMethod method in document.AsyncMethods)
        {
            if (method is null || method.LexicalScopes is null) return false;
            if (document.SchemaVersion < 27)
            {
                if (method.LexicalScopes.Count != 0) return false;
                continue;
            }
            if (!(document.SchemaVersion == 27 && document.SemanticVersion == "1.31")
                && !(document.SchemaVersion == 28 && document.SemanticVersion == "1.32")
                && !(document.SchemaVersion == SemanticContract.CurrentSchemaVersion && document.SemanticVersion == SemanticContract.CurrentSemanticVersion)) return false;
            if (method.LexicalScopes.Count == 0)
            {
                if (document.ClosureEnvironments.Any(environment => environment?.OwnerMethodSymbolId == method.MethodSymbolId)) return false;
                continue;
            }
            if (method.Lowering != SemanticAsyncMethod.ContinuationCfgLowering || method.Span is null
                || method.Segments is null || method.Segments.Count == 0
                || method.Segments.Count > SemanticAsyncMethod.MaximumControlFlowSegments
                || method.LexicalScopes.Count > SemanticAsyncMethod.MaximumStructuredFlowNodes
                || method.EntrySegmentOrdinal < 0 || method.EntrySegmentOrdinal >= method.Segments.Count
                || method.Segments.Where((segment, index) => segment is null || segment.Ordinal != index || segment.Transfer is null || segment.Span is null).Any()) return false;
            foreach (SemanticAsyncSegment segment in method.Segments)
            {
                SemanticAsyncControlTransfer transfer = segment.Transfer!;
                if (transfer.Kind is not (SemanticAsyncMethod.GotoTransferKind or SemanticAsyncMethod.BranchTransferKind
                    or SemanticAsyncMethod.AwaitTransferKind or SemanticAsyncMethod.ReturnTransferKind)
                    || Targets(transfer).Any(target => target < 0 || target >= method.Segments.Count)) return false;
            }
            HashSet<string> ids = new(StringComparer.Ordinal);
            foreach (SemanticAsyncLexicalScope scope in method.LexicalScopes)
            {
                if (scope is null || scope.Kind is not ("activation" or "block_entry" or "for_entry" or "foreach_iteration")
                    || scope.Ordinal < 0 || scope.Id != SemanticClosureEnvironment.GetId(method.MethodSymbolId, scope.Kind, scope.Ordinal)
                    || !ids.Add(scope.Id) || scope.Span is null || scope.Span.Start < method.Span.Start || scope.Span.Length <= 0
                    || (long)scope.Span.Start + scope.Span.Length > (long)method.Span.Start + method.Span.Length
                    || scope.Segments is null
                    || !scope.Segments.SequenceEqual(scope.Segments.Distinct().Order())
                    || scope.Segments.Any(segment => segment < 0 || segment >= method.Segments.Count)
                    || scope.Entries is null || scope.Entries.Any(entry => entry is null)
                    || !scope.Entries.SequenceEqual(GetEntries(method.Segments, method.EntrySegmentOrdinal, scope.Segments))) return false;
                bool Contains(SemanticSpan span) => span.Start >= scope.Span.Start
                    && (long)span.Start + span.Length <= (long)scope.Span.Start + scope.Span.Length;
                if (scope.Segments.Any(segment => !Contains(method.Segments[segment].Span))
                    || scope.Segments.Count == 0 && method.Segments.Any(segment => Contains(segment.Span))) return false;
                if (scope.Kind == "activation" && (scope.Ordinal != 0 || scope.Span != method.Span
                    || !scope.Segments.SequenceEqual(Enumerable.Range(0, method.Segments.Count)))) return false;
            }
            if (method.LexicalScopes.Count(scope => scope.Kind == "activation") != 1
                || !method.LexicalScopes.Select(scope => scope.Id).SequenceEqual(ids.OrderBy(id => id, StringComparer.Ordinal))) return false;
            foreach (SemanticClosureEnvironment environment in document.ClosureEnvironments.Where(environment => environment?.OwnerMethodSymbolId == method.MethodSymbolId))
            {
                SemanticAsyncLexicalScope? scope = method.LexicalScopes.SingleOrDefault(scope => scope.Id == environment.Id);
                if (scope is null || scope.Span != environment.Span || environment.Allocation is not null) return false;
            }
        }
        return true;
    }

    public static SemanticClosureEntry[] GetEntries(IReadOnlyList<SemanticAsyncSegment> segments, int entry, IReadOnlyList<int> members)
    {
        HashSet<int> inside = members.ToHashSet();
        IEnumerable<SemanticClosureEntry> edges = segments.Where(segment => !inside.Contains(segment.Ordinal))
            .SelectMany(segment => Targets(segment.Transfer!).Where(inside.Contains)
                .Select(target => new SemanticClosureEntry(segment.Ordinal, target)));
        if (inside.Contains(entry)) edges = edges.Append(new(null, entry));
        return edges.Distinct().OrderBy(edge => edge.SourceBlockOrdinal).ThenBy(edge => edge.DestinationBlockOrdinal).ToArray();
    }

    public static IEnumerable<int> Targets(SemanticAsyncControlTransfer transfer)
    {
        if (transfer.Kind is SemanticAsyncMethod.GotoTransferKind or SemanticAsyncMethod.AwaitTransferKind or SemanticAsyncMethod.BranchTransferKind)
            yield return transfer.PrimaryTarget;
        if (transfer.Kind == SemanticAsyncMethod.BranchTransferKind) yield return transfer.SecondaryTarget;
    }
}
