using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.CSharpSemantic;

public sealed record SemanticAsyncLocalState(
    string SymbolId,
    string TypeId,
    int DeclarationSegment,
    SemanticSpan Span);

public sealed record SemanticAsyncStateFlowIssue(
    string SymbolId,
    SemanticSpan Span,
    string Message);

public sealed record SemanticAsyncStateFlowAnalysis(
    IReadOnlyList<SemanticAsyncLocalState> Locals,
    IReadOnlyDictionary<int, IReadOnlyList<SemanticAsyncStateSlot>> SlotsByAwaitSegment,
    IReadOnlyList<SemanticAsyncStateFlowIssue> Issues);

public static class SemanticAsyncStateFlowAnalyzer
{
    public static SemanticAsyncStateFlowAnalysis Analyze(
        IReadOnlyList<SemanticAsyncSegment> segments)
    {
        ArgumentNullException.ThrowIfNull(segments);

        Dictionary<string, SemanticAsyncLocalState> locals = new(StringComparer.Ordinal);
        List<SemanticAsyncStateFlowIssue> issues = new();
        foreach (SemanticAsyncSegment segment in segments)
        {
            foreach (SemanticAsyncStatement statement in segment.Statements)
            {
                if (statement.TargetSymbolId is { } targetSymbolId)
                {
                    AddLocal(
                        locals,
                        issues,
                        targetSymbolId,
                        statement.Operation.TypeId,
                        segment.Ordinal,
                        statement.Operation.Span);
                }
                foreach (SemanticOperation operation in EnumerateOperations(statement.Operation))
                {
                    if (operation.Kind == SemanticAsyncMethod.LocalDeclarationOperationKind)
                    {
                        AddLocal(
                            locals,
                            issues,
                            operation.SymbolId,
                            operation.TypeId,
                            segment.Ordinal,
                            operation.Span);
                    }
                }
            }

            if (segment.AwaitSite is
                { ResultSymbolId: { } resultSymbolId, ResultTypeId: { } resultTypeId } awaitSite)
            {
                AddLocal(
                    locals,
                    issues,
                    resultSymbolId,
                    resultTypeId,
                    segment.Ordinal + 1,
                    awaitSite.Span);
            }
        }

        foreach (SemanticAsyncSegment segment in segments)
        {
            IEnumerable<SemanticOperation> roots = segment.Statements
                .Select(statement => statement.Operation)
                .Concat(segment.AwaitSite?.Arguments ?? Array.Empty<SemanticOperation>());
            if (segment.AwaitSite?.CancellationToken is { } cancellationToken)
            {
                roots = roots.Append(cancellationToken);
            }
            foreach (SemanticOperation reference in roots
                .SelectMany(EnumerateOperations)
                .Where(operation => operation.Kind == "local_reference"
                    && operation.SymbolId is not null))
            {
                SemanticAsyncLocalState? local = locals.GetValueOrDefault(reference.SymbolId!);
                if (local is not null && segment.Ordinal < local.DeclarationSegment)
                {
                    issues.Add(new SemanticAsyncStateFlowIssue(
                        local.SymbolId,
                        reference.Span,
                        $"Async local '{local.SymbolId}' is used before its declaration segment."));
                }
            }
        }

        HashSet<string> localIds = locals.Keys.ToHashSet(StringComparer.Ordinal);
        Dictionary<int, IReadOnlyList<SemanticAsyncStateSlot>> slotsByAwait = new();
        HashSet<string> live = new(StringComparer.Ordinal);
        for (int index = segments.Count - 1; index >= 0; --index)
        {
            SemanticAsyncSegment segment = segments[index];
            if (segment.AwaitSite is not null)
            {
                slotsByAwait.Add(
                    segment.Ordinal,
                    live.Where(symbolId =>
                            locals.TryGetValue(symbolId, out SemanticAsyncLocalState? local)
                            && local.DeclarationSegment <= segment.Ordinal)
                        .OrderBy(symbolId => symbolId, StringComparer.Ordinal)
                        .Select(symbolId => new SemanticAsyncStateSlot(
                            symbolId,
                            locals[symbolId].TypeId))
                        .ToArray());

                if (segment.AwaitSite.CancellationToken is { } cancellationToken)
                {
                    live = TransferValue(cancellationToken, live, localIds);
                }
                for (int argument = segment.AwaitSite.Arguments.Count - 1; argument >= 0; --argument)
                {
                    live = TransferValue(segment.AwaitSite.Arguments[argument], live, localIds);
                }
            }

            for (int statementIndex = segment.Statements.Count - 1; statementIndex >= 0; --statementIndex)
            {
                SemanticAsyncStatement statement = segment.Statements[statementIndex];
                if (statement.TargetSymbolId is { } targetSymbolId)
                {
                    live = Clone(live);
                    live.Remove(targetSymbolId);
                    live = TransferValue(statement.Operation, live, localIds);
                }
                else
                {
                    live = TransferFlow(
                        statement.Operation,
                        live,
                        localIds,
                        FlowTargets.Method);
                }
            }

            if (index > 0 && segments[index - 1].AwaitSite?.ResultSymbolId is { } incomingResult)
            {
                live = Clone(live);
                live.Remove(incomingResult);
            }
        }

        return new SemanticAsyncStateFlowAnalysis(
            locals.Values
                .OrderBy(local => local.SymbolId, StringComparer.Ordinal)
                .ToArray(),
            slotsByAwait,
            issues
                .OrderBy(issue => issue.Span.Start)
                .ThenBy(issue => issue.SymbolId, StringComparer.Ordinal)
                .ToArray());
    }

    public static SemanticAsyncStateFlowAnalysis AnalyzeControlFlow(
        IReadOnlyList<SemanticAsyncSegment> segments)
    {
        ArgumentNullException.ThrowIfNull(segments);

        Dictionary<string, SemanticAsyncLocalState> locals = new(StringComparer.Ordinal);
        List<SemanticAsyncStateFlowIssue> issues = new();
        foreach (SemanticAsyncSegment segment in segments)
        {
            foreach (SemanticAsyncStatement statement in segment.Statements)
            {
                if (statement.TargetSymbolId is { } targetSymbolId)
                {
                    AddLocal(
                        locals,
                        issues,
                        targetSymbolId,
                        statement.Operation.TypeId,
                        segment.Ordinal,
                        statement.Operation.Span);
                }
                foreach (SemanticOperation operation in EnumerateOperations(statement.Operation))
                {
                    if (operation.Kind == SemanticAsyncMethod.LocalDeclarationOperationKind)
                    {
                        AddLocal(
                            locals,
                            issues,
                            operation.SymbolId,
                            operation.TypeId,
                            segment.Ordinal,
                            operation.Span);
                    }
                }
            }

            if (segment.AwaitSite is
                { ResultSymbolId: { } resultSymbolId, ResultTypeId: { } resultTypeId } awaitSite
                && segment.Transfer is { Kind: SemanticAsyncMethod.AwaitTransferKind } transfer)
            {
                AddLocal(
                    locals,
                    issues,
                    resultSymbolId,
                    resultTypeId,
                    transfer.PrimaryTarget,
                    awaitSite.Span);
            }
        }

        if (segments.Select(segment => segment.Ordinal)
            .Where(ordinal => ordinal < 0 || ordinal >= segments.Count)
            .Any())
        {
            issues.Add(new SemanticAsyncStateFlowIssue(
                string.Empty,
                segments.FirstOrDefault()?.Span ?? new SemanticSpan(0, 0, 0, 0, 0, 0),
                "Async CFG contains an invalid segment ordinal."));
        }

        HashSet<string> localIds = locals.Keys.ToHashSet(StringComparer.Ordinal);
        Dictionary<int, HashSet<string>> liveIn = segments.ToDictionary(
            segment => segment.Ordinal,
            _ => new HashSet<string>(StringComparer.Ordinal));
        int limit = Math.Max(1, segments.Count * (localIds.Count + 2));
        for (int iteration = 0; iteration < limit; ++iteration)
        {
            bool changed = false;
            for (int index = segments.Count - 1; index >= 0; --index)
            {
                SemanticAsyncSegment segment = segments[index];
                HashSet<string> candidate = TransferControlFlowSegment(
                    segment,
                    liveIn,
                    localIds,
                    issues);
                if (!candidate.SetEquals(liveIn[segment.Ordinal]))
                {
                    liveIn[segment.Ordinal] = candidate;
                    changed = true;
                }
            }
            if (!changed)
            {
                break;
            }
            if (iteration == limit - 1)
            {
                issues.Add(new SemanticAsyncStateFlowIssue(
                    string.Empty,
                    segments.First().Span,
                    "Async CFG liveness did not converge within its bounded local lattice."));
            }
        }

        Dictionary<int, IReadOnlyList<SemanticAsyncStateSlot>> slotsByAwait = new();
        foreach (SemanticAsyncSegment segment in segments.Where(item => item.AwaitSite is not null))
        {
            if (segment.Transfer is not
                { Kind: SemanticAsyncMethod.AwaitTransferKind } transfer
                || !liveIn.TryGetValue(transfer.PrimaryTarget, out HashSet<string>? resumeLive))
            {
                issues.Add(new SemanticAsyncStateFlowIssue(
                    string.Empty,
                    segment.Span,
                    "Async CFG await segment has no valid resume successor."));
                continue;
            }

            HashSet<string> slots = Clone(resumeLive);
            if (segment.AwaitSite!.ResultSymbolId is { } resultSymbolId)
            {
                slots.Remove(resultSymbolId);
            }
            slotsByAwait[segment.Ordinal] = slots
                .Where(locals.ContainsKey)
                .OrderBy(symbolId => symbolId, StringComparer.Ordinal)
                .Select(symbolId => new SemanticAsyncStateSlot(
                    symbolId,
                    locals[symbolId].TypeId))
                .ToArray();
        }

        return new SemanticAsyncStateFlowAnalysis(
            locals.Values
                .OrderBy(local => local.SymbolId, StringComparer.Ordinal)
                .ToArray(),
            slotsByAwait,
            issues
                .GroupBy(issue => (issue.SymbolId, issue.Span.Start, issue.Message))
                .Select(group => group.First())
                .OrderBy(issue => issue.Span.Start)
                .ThenBy(issue => issue.SymbolId, StringComparer.Ordinal)
                .ToArray());
    }

    private static HashSet<string> TransferControlFlowSegment(
        SemanticAsyncSegment segment,
        IReadOnlyDictionary<int, HashSet<string>> liveIn,
        IReadOnlySet<string> localIds,
        ICollection<SemanticAsyncStateFlowIssue> issues)
    {
        SemanticAsyncControlTransfer? transfer = segment.Transfer;
        HashSet<string> live = transfer?.Kind switch
        {
            SemanticAsyncMethod.ReturnTransferKind =>
                new HashSet<string>(StringComparer.Ordinal),
            SemanticAsyncMethod.GotoTransferKind =>
                GetSuccessorLive(segment, transfer.PrimaryTarget, liveIn, issues),
            SemanticAsyncMethod.BranchTransferKind => TransferValue(
                transfer.Condition!,
                Union(
                    GetSuccessorLive(segment, transfer.PrimaryTarget, liveIn, issues),
                    GetSuccessorLive(segment, transfer.SecondaryTarget, liveIn, issues)),
                localIds),
            SemanticAsyncMethod.AwaitTransferKind => TransferAwait(
                segment,
                transfer,
                liveIn,
                localIds,
                issues),
            _ => new HashSet<string>(StringComparer.Ordinal),
        };

        for (int index = segment.Statements.Count - 1; index >= 0; --index)
        {
            SemanticAsyncStatement statement = segment.Statements[index];
            if (statement.TargetSymbolId is { } targetSymbolId)
            {
                live = Clone(live);
                live.Remove(targetSymbolId);
                live = TransferValue(statement.Operation, live, localIds);
            }
            else
            {
                live = TransferFlow(statement.Operation, live, localIds, FlowTargets.Method);
            }
        }
        return live;
    }

    private static HashSet<string> TransferAwait(
        SemanticAsyncSegment segment,
        SemanticAsyncControlTransfer transfer,
        IReadOnlyDictionary<int, HashSet<string>> liveIn,
        IReadOnlySet<string> localIds,
        ICollection<SemanticAsyncStateFlowIssue> issues)
    {
        HashSet<string> live = GetSuccessorLive(
            segment,
            transfer.PrimaryTarget,
            liveIn,
            issues);
        if (segment.AwaitSite?.ResultSymbolId is { } resultSymbolId)
        {
            live.Remove(resultSymbolId);
        }
        if (segment.AwaitSite?.CancellationToken is { } cancellationToken)
        {
            live = TransferValue(cancellationToken, live, localIds);
        }
        for (int index = (segment.AwaitSite?.Arguments.Count ?? 0) - 1; index >= 0; --index)
        {
            live = TransferValue(segment.AwaitSite!.Arguments[index], live, localIds);
        }
        return live;
    }

    private static HashSet<string> GetSuccessorLive(
        SemanticAsyncSegment segment,
        int target,
        IReadOnlyDictionary<int, HashSet<string>> liveIn,
        ICollection<SemanticAsyncStateFlowIssue> issues)
    {
        if (liveIn.TryGetValue(target, out HashSet<string>? successor))
        {
            return Clone(successor);
        }
        issues.Add(new SemanticAsyncStateFlowIssue(
            string.Empty,
            segment.Span,
            $"Async CFG segment {segment.Ordinal} targets missing segment {target}."));
        return new HashSet<string>(StringComparer.Ordinal);
    }

    private static HashSet<string> TransferFlow(
        SemanticOperation operation,
        HashSet<string> liveAfter,
        IReadOnlySet<string> localIds,
        FlowTargets targets)
    {
        switch (operation.Kind)
        {
            case SemanticAsyncMethod.BlockOperationKind:
            {
                HashSet<string> live = Clone(liveAfter);
                for (int index = operation.Children.Count - 1; index >= 0; --index)
                {
                    live = TransferFlow(operation.Children[index], live, localIds, targets);
                }
                return live;
            }

            case SemanticAsyncMethod.LocalDeclarationOperationKind:
            {
                HashSet<string> live = Clone(liveAfter);
                if (operation.SymbolId is { } symbolId)
                {
                    live.Remove(symbolId);
                }
                if (operation.Children.Count == 1)
                {
                    live = TransferValue(operation.Children[0], live, localIds);
                }
                return live;
            }

            case SemanticAsyncMethod.IfOperationKind:
            {
                HashSet<string> whenTrue = TransferFlow(
                    operation.Children[1],
                    liveAfter,
                    localIds,
                    targets);
                HashSet<string> whenFalse = operation.Children.Count == 3
                    ? TransferFlow(operation.Children[2], liveAfter, localIds, targets)
                    : Clone(liveAfter);
                return TransferValue(
                    operation.Children[0],
                    Union(whenTrue, whenFalse),
                    localIds);
            }

            case SemanticAsyncMethod.WhileOperationKind:
                return TransferWhile(operation, liveAfter, localIds, targets);

            case SemanticAsyncMethod.DoWhileOperationKind:
                return TransferDoWhile(operation, liveAfter, localIds, targets);

            case SemanticAsyncMethod.ForOperationKind:
                return TransferFor(operation, liveAfter, localIds, targets);

            case SemanticAsyncMethod.BreakOperationKind:
                return targets.Break is null ? new HashSet<string>(StringComparer.Ordinal) : Clone(targets.Break);

            case SemanticAsyncMethod.ContinueOperationKind:
                return targets.Continue is null ? new HashSet<string>(StringComparer.Ordinal) : Clone(targets.Continue);

            case SemanticAsyncMethod.ReturnOperationKind:
                return new HashSet<string>(StringComparer.Ordinal);

            case SemanticAsyncMethod.EarlyReturnGuardOperationKind:
                return TransferValue(operation.Children[0], Clone(liveAfter), localIds);

            default:
                return TransferValue(operation, liveAfter, localIds);
        }
    }

    private static HashSet<string> TransferWhile(
        SemanticOperation operation,
        HashSet<string> liveAfter,
        IReadOnlySet<string> localIds,
        FlowTargets outerTargets)
    {
        HashSet<string> header = new(StringComparer.Ordinal);
        int limit = localIds.Count + 2;
        for (int iteration = 0; iteration < limit; ++iteration)
        {
            HashSet<string> body = TransferFlow(
                operation.Children[1],
                header,
                localIds,
                outerTargets with { Break = liveAfter, Continue = header });
            HashSet<string> candidate = TransferValue(
                operation.Children[0],
                Union(liveAfter, body),
                localIds);
            if (candidate.SetEquals(header))
            {
                return candidate;
            }
            header = candidate;
        }
        return header;
    }

    private static HashSet<string> TransferDoWhile(
        SemanticOperation operation,
        HashSet<string> liveAfter,
        IReadOnlySet<string> localIds,
        FlowTargets outerTargets)
    {
        HashSet<string> bodyEntry = new(StringComparer.Ordinal);
        int limit = localIds.Count + 2;
        for (int iteration = 0; iteration < limit; ++iteration)
        {
            HashSet<string> condition = TransferValue(
                operation.Children[1],
                Union(liveAfter, bodyEntry),
                localIds);
            HashSet<string> candidate = TransferFlow(
                operation.Children[0],
                condition,
                localIds,
                outerTargets with { Break = liveAfter, Continue = condition });
            if (candidate.SetEquals(bodyEntry))
            {
                return candidate;
            }
            bodyEntry = candidate;
        }
        return bodyEntry;
    }

    private static HashSet<string> TransferFor(
        SemanticOperation operation,
        HashSet<string> liveAfter,
        IReadOnlySet<string> localIds,
        FlowTargets outerTargets)
    {
        HashSet<string> header = new(StringComparer.Ordinal);
        int limit = localIds.Count + 2;
        for (int iteration = 0; iteration < limit; ++iteration)
        {
            HashSet<string> increment = TransferFlow(
                operation.Children[2],
                header,
                localIds,
                outerTargets);
            HashSet<string> body = TransferFlow(
                operation.Children[3],
                increment,
                localIds,
                outerTargets with { Break = liveAfter, Continue = increment });
            HashSet<string> candidate = TransferValue(
                operation.Children[1],
                Union(liveAfter, body),
                localIds);
            if (candidate.SetEquals(header))
            {
                return TransferFlow(operation.Children[0], candidate, localIds, outerTargets);
            }
            header = candidate;
        }
        return TransferFlow(operation.Children[0], header, localIds, outerTargets);
    }

    private static HashSet<string> TransferValue(
        SemanticOperation operation,
        HashSet<string> liveAfter,
        IReadOnlySet<string> localIds)
    {
        HashSet<string> live = Clone(liveAfter);
        if (operation.Kind == "assignment"
            && operation.Children.Count == 2
            && TryGetDirectLocal(operation.Children[0], localIds, out string? assignedLocal))
        {
            live.Remove(assignedLocal!);
            return TransferValue(operation.Children[1], live, localIds);
        }

        for (int index = operation.Children.Count - 1; index >= 0; --index)
        {
            live = TransferValue(operation.Children[index], live, localIds);
        }
        if (operation.Kind == "local_reference"
            && operation.SymbolId is { } symbolId
            && localIds.Contains(symbolId))
        {
            live.Add(symbolId);
        }
        return live;
    }

    private static bool TryGetDirectLocal(
        SemanticOperation operation,
        IReadOnlySet<string> localIds,
        out string? symbolId)
    {
        if (operation.Kind == "local_reference"
            && operation.SymbolId is { } direct
            && localIds.Contains(direct))
        {
            symbolId = direct;
            return true;
        }
        if (operation.Kind is "parenthesized" or "conversion"
            && operation.Children.Count == 1)
        {
            return TryGetDirectLocal(operation.Children[0], localIds, out symbolId);
        }
        symbolId = null;
        return false;
    }

    private static void AddLocal(
        IDictionary<string, SemanticAsyncLocalState> locals,
        ICollection<SemanticAsyncStateFlowIssue> issues,
        string? symbolId,
        string? typeId,
        int declarationSegment,
        SemanticSpan span)
    {
        if (string.IsNullOrWhiteSpace(symbolId) || string.IsNullOrWhiteSpace(typeId))
        {
            issues.Add(new SemanticAsyncStateFlowIssue(
                symbolId ?? string.Empty,
                span,
                "Async local declaration has no stable symbol or type identity."));
            return;
        }
        if (!locals.TryAdd(
            symbolId,
            new SemanticAsyncLocalState(symbolId, typeId, declarationSegment, span)))
        {
            issues.Add(new SemanticAsyncStateFlowIssue(
                symbolId,
                span,
                $"Async local '{symbolId}' is declared more than once."));
        }
    }

    private static IEnumerable<SemanticOperation> EnumerateOperations(
        SemanticOperation operation)
    {
        yield return operation;
        foreach (SemanticOperation child in operation.Children)
        {
            foreach (SemanticOperation descendant in EnumerateOperations(child))
            {
                yield return descendant;
            }
        }
    }

    private static HashSet<string> Clone(IEnumerable<string> source)
    {
        return new HashSet<string>(source, StringComparer.Ordinal);
    }

    private static HashSet<string> Union(
        IEnumerable<string> left,
        IEnumerable<string> right)
    {
        HashSet<string> result = Clone(left);
        result.UnionWith(right);
        return result;
    }

    private sealed record FlowTargets(
        IReadOnlySet<string>? Break,
        IReadOnlySet<string>? Continue)
    {
        public static FlowTargets Method { get; } = new(null, null);
    }
}
