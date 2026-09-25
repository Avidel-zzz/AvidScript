using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.CSharpSemantic;

// The same reader contract protects downstream consumers and independently
// verifies that no caller stack reference becomes a persistent async input.
public static class SemanticAsyncInvocationValidator
{
    public const int MaximumTaskLocalsPerMethod = 8;

    public static bool IsValid(SemanticDocument document)
    {
        if (document.AsyncMethods is null || document.Callables is null || document.Symbols is null
            || document.Types is null || document.TypeShapes is null
            || document.Callables.Any(callable => callable is null || string.IsNullOrWhiteSpace(callable.MethodSymbolId))
            || document.Callables.Select(callable => callable.MethodSymbolId).Distinct(StringComparer.Ordinal).Count() != document.Callables.Count)
            return false;
        bool taskResultContract = document.SchemaVersion == SemanticContract.TaskResultSchemaVersion
            && document.SemanticVersion == SemanticContract.TaskResultSemanticVersion;
        bool taskLocalContract = document.SchemaVersion == SemanticContract.TaskLocalSchemaVersion
            && document.SemanticVersion == SemanticContract.TaskLocalSemanticVersion;
        bool taskAssignmentContract = document.SchemaVersion == SemanticContract.TaskAssignmentSchemaVersion
            && document.SemanticVersion == SemanticContract.TaskAssignmentSemanticVersion;
        bool taskExistingLocalContract = document.SchemaVersion == SemanticContract.TaskExistingLocalSchemaVersion
            && document.SemanticVersion == SemanticContract.TaskExistingLocalSemanticVersion;
        bool taskAliasContract = document.SchemaVersion == SemanticContract.TaskAliasSchemaVersion
            && document.SemanticVersion == SemanticContract.TaskAliasSemanticVersion;
        bool combinedContract =
            (document.SchemaVersion == SemanticContract.TaskLanguageErrorSchemaVersion
                && document.SemanticVersion == SemanticContract.TaskLanguageErrorSemanticVersion)
            || (document.SchemaVersion == SemanticContract.AsyncLanguageErrorSchemaVersion
                && document.SemanticVersion == SemanticContract.AsyncLanguageErrorSemanticVersion);
        if (!SemanticAsyncErrorPlanValidator.IsValid(document)) return false;
        if (taskResultContract && !document.AsyncMethods.Any(method => method?.TaskResultTypeId is not null
                || method?.Segments?.Any(segment => segment?.AwaitSite?.TaskCallableId is not null) == true))
            return false;
        if (taskLocalContract && !document.AsyncMethods.Any(method => method?.Segments?.Any(segment =>
                segment?.AwaitSite?.TaskLocalSymbolId is not null) == true))
            return false;
        if (taskAssignmentContract && !document.AsyncMethods.Any(method => method?.Segments?.Any(segment =>
                segment?.AwaitSite?.ResultStorageKind == "static_field") == true))
            return false;
        if (taskExistingLocalContract && !document.AsyncMethods.Any(method => method?.Segments?.Any(segment =>
                segment?.AwaitSite?.ResultStorageKind == "existing_local") == true))
            return false;
        if (taskAliasContract && !document.AsyncMethods.Any(method => method?.TaskLocalSymbolIds is { Count: > 0 }))
            return false;
        if (!(taskAliasContract || combinedContract)
            && document.AsyncMethods.Any(method => method?.TaskLocalSymbolIds is not null))
            return false;
        var callables = document.Callables.ToDictionary(callable => callable.MethodSymbolId, StringComparer.Ordinal);
        foreach (SemanticAsyncMethod method in document.AsyncMethods)
        {
            if (method is null || string.IsNullOrWhiteSpace(method.MethodSymbolId) || method.InvocationInputs is null
                || !callables.TryGetValue(method.MethodSymbolId, out SemanticCallable? callable)
                || callable.Parameters is null || callable.Parameters.Any(parameter => parameter is null
                    || string.IsNullOrWhiteSpace(parameter.SymbolId) || string.IsNullOrWhiteSpace(parameter.TypeId))
                || !callable.HasBody || callable.IsConstructor || callable.Import is not null
                || callable.Export?.Name != method.ExportName
                || (method.TaskResultTypeId is null
                    ? callable.ReturnTypeId != "type:void"
                    : !IsSupportedTaskResult(document, callable.ReturnTypeId, method.TaskResultTypeId)
                        || method.ExportName is not null))
                return false;
            if (method.TaskResultTypeId is not null
                && (!(taskResultContract || taskLocalContract || taskAssignmentContract || taskExistingLocalContract || taskAliasContract || combinedContract)
                    || method.Lowering != SemanticAsyncMethod.ContinuationCfgLowering)) return false;
            if (document.SchemaVersion < 28)
            {
                if (method.InvocationInputs.Count != 0 || method.ExportName is null
                    || !callable.IsStatic || callable.Parameters.Count != 0) return false;
                continue;
            }
            if (!(document.SchemaVersion == 28 && document.SemanticVersion == "1.32")
                && !(document.SchemaVersion == 29 && document.SemanticVersion == "1.33")
                && !SemanticContract.IsCurrentOrPrevious(document.SchemaVersion, document.SemanticVersion)) return false;
            if (method.ExportName is not null)
            {
                if (string.IsNullOrWhiteSpace(method.ExportName) || !callable.IsStatic
                    || callable.Parameters.Count != 0 || method.InvocationInputs.Count != 0) return false;
                continue;
            }
            if (method.Lowering != SemanticAsyncMethod.ContinuationCfgLowering
                || callable.Parameters.Any(parameter => parameter.RefKind != "none")
                || method.InvocationInputs.Count > 64
                || !document.Types.Any(type => type?.Id == callable.ContainingTypeId && type.Kind == "class")) return false;
            var expected = callable.Parameters.Select(parameter => new SemanticAsyncStateSlot(parameter.SymbolId, parameter.TypeId))
                .Concat(callable.IsStatic ? Array.Empty<SemanticAsyncStateSlot>() : new[] {
                    new SemanticAsyncStateSlot(SemanticAsyncMethod.ReceiverSymbol(callable.MethodSymbolId), callable.ContainingTypeId) })
                .OrderBy(slot => slot.SymbolId, StringComparer.Ordinal).ToArray();
            if (!method.InvocationInputs.SequenceEqual(expected)
                || expected.Select(slot => slot.SymbolId).Distinct(StringComparer.Ordinal).Count() != expected.Length
                || expected.Any(slot => !document.Types.Any(type => type?.Id == slot.TypeId))
                || callable.Parameters.Any(parameter => !document.Symbols.Any(symbol => symbol?.Id == parameter.SymbolId
                    && symbol.Kind == "parameter" && symbol.TypeId == parameter.TypeId && symbol.ContainingSymbolId == callable.MethodSymbolId))) return false;
            if (method.Segments is null || method.Segments.Any(segment => segment is null)) return false;
            IReadOnlyDictionary<string, string> taskProducers = new Dictionary<string, string>();
            if (taskLocalContract || taskAssignmentContract || taskExistingLocalContract || taskAliasContract || combinedContract)
            {
                HashSet<string> taskTypes = document.Types.Where(type =>
                        type.CanonicalName == "global::System.Threading.Tasks.Task<int>")
                    .Select(type => type.Id).ToHashSet(StringComparer.Ordinal);
                string[] declaredTasks = document.Symbols.Where(symbol => symbol.Kind == "local"
                        && symbol.ContainingSymbolId == method.MethodSymbolId
                        && symbol.TypeId is { } typeId && taskTypes.Contains(typeId))
                    .Select(symbol => symbol.Id).OrderBy(id => id, StringComparer.Ordinal).ToArray();
                string[] awaitedTasks = method.Segments.Select(segment =>
                        segment.AwaitSite?.TaskLocalSymbolId).OfType<string>()
                    .Distinct(StringComparer.Ordinal).OrderBy(id => id, StringComparer.Ordinal).ToArray();
                string[] ownedTasks = method.TaskLocalSymbolIds?.OrderBy(id => id, StringComparer.Ordinal).ToArray()
                    ?? awaitedTasks;
                if (method.TaskLocalSymbolIds is { Count: 0 }
                    || declaredTasks.Length > MaximumTaskLocalsPerMethod
                    || !declaredTasks.SequenceEqual(ownedTasks)
                    || method.TaskLocalSymbolIds is { } listed
                        && (listed.Count == 0 || listed.Distinct(StringComparer.Ordinal).Count() != listed.Count
                            || !(taskAliasContract || combinedContract))) return false;
                if (ownedTasks.Length != 0)
                {
                    if (!TryGetTaskLocalBindings(method, ownedTasks,
                            taskAliasContract || method.TaskLocalSymbolIds is not null,
                            out taskProducers, out IReadOnlyDictionary<string, string> aliases)
                        || !TaskLocalReferencesAreRestricted(method, ownedTasks)
                        || !AllTaskLocalsReachAwait(awaitedTasks, ownedTasks, aliases)) return false;
                    if (method.TaskLocalSymbolIds is not null && aliases.Count == 0) return false;
                }
                SemanticAsyncStateFlowAnalysis flow = SemanticAsyncStateFlowAnalyzer.AnalyzeControlFlow(method.Segments);
                if (flow.Issues.Count != 0 || method.Segments.Where(segment => segment.AwaitSite is not null)
                    .Any(segment => !MergeStateSlots(
                            flow.SlotsByAwaitSegment.GetValueOrDefault(segment.Ordinal)
                                ?? Array.Empty<SemanticAsyncStateSlot>(), method.InvocationInputs)
                        .SequenceEqual(segment.AwaitSite!.StateFrame?.Slots
                            ?? Array.Empty<SemanticAsyncStateSlot>()))) return false;
            }
            if (method.TaskResultTypeId is not null
                && method.Segments.Any(segment => segment.Transfer is
                    { Kind: SemanticAsyncMethod.ReturnTransferKind, Condition: { } value }
                    && value.TypeId != method.TaskResultTypeId)) return false;
            foreach (SemanticAsyncAwaitSite site in method.Segments.Where(segment => segment.AwaitSite is not null).Select(segment => segment.AwaitSite!))
            {
                if (expected.Length != 0 && (site.StateFrame?.Slots is null
                    || expected.Any(slot => site.StateFrame.Slots.Count(candidate => candidate == slot) != 1))) return false;
                if (site.ProducerKind is "task_call" or "task_local")
                {
                    if (!(taskResultContract || taskLocalContract || taskAssignmentContract || taskExistingLocalContract || taskAliasContract || combinedContract)
                        || site.TaskCallableId is null
                        || !callables.TryGetValue(site.TaskCallableId, out SemanticCallable? target)
                        || !target.IsStatic
                        || !IsSupportedTaskResult(document, target.ReturnTypeId, site.ResultTypeId ?? "")
                        || !document.AsyncMethods.Any(producer => producer.MethodSymbolId == site.TaskCallableId
                            && producer.TaskResultTypeId == site.ResultTypeId)
                        || site.PayloadKind != "task_result"
                        || site.PayloadValueTypeId != site.ResultTypeId
                        || site.ResultStorageKind is not (null or "static_field" or "existing_local")
                        || site.ResultStorageKind == "static_field"
                            && (!(taskAssignmentContract || taskExistingLocalContract || taskAliasContract || combinedContract) || site.ResultSymbolId is null
                                || !IsWritableStaticResultField(document, callable,
                                    site.ResultSymbolId, site.ResultTypeId))
                        || site.ResultStorageKind == "existing_local"
                            && (!(taskExistingLocalContract || taskAliasContract || combinedContract) || site.ResultSymbolId is null
                                || !HasEarlierResultLocal(method, site))
                        || site.ResultStorageKind is null
                            && site.ResultSymbolId is { } resultSymbolId
                            && !document.Symbols.Any(symbol => symbol?.Id == resultSymbolId
                                && symbol.Kind == "local" && symbol.TypeId == site.ResultTypeId
                                && symbol.ContainingSymbolId == method.MethodSymbolId)
                        || site.Arguments is null
                        || site.CancellationToken is not null
                        || site.BindingOrdinal != -1
                        || site.PayloadDescriptorTypeId is not null) return false;
                    if (site.ProducerKind == "task_call")
                    {
                        if (site.TaskLocalSymbolId is not null
                            || site.Arguments.Count != target.Parameters.Count
                            || site.Arguments.Where((argument, index) => argument is null
                                || target.Parameters[index].RefKind != "none"
                                || argument.TypeId != target.Parameters[index].TypeId).Any()) return false;
                    }
                    else
                    {
                        if (!(taskLocalContract || taskAssignmentContract || taskExistingLocalContract || taskAliasContract || combinedContract)
                            || site.TaskLocalSymbolId is not { } taskLocalId
                            || site.Arguments.Count != 1
                            || site.Arguments[0] is not
                                { Kind: "local_reference", SymbolId: var referencedId, TypeId: var taskTypeId }
                            || referencedId != taskLocalId || taskTypeId != target.ReturnTypeId
                            || !document.Symbols.Any(symbol => symbol?.Id == taskLocalId
                                && symbol.Kind == "local" && symbol.TypeId == target.ReturnTypeId)
                            || !taskProducers.TryGetValue(taskLocalId, out string? producerId)
                            || producerId != site.TaskCallableId) return false;
                    }
                }
                else if (site.TaskCallableId is not null || site.TaskLocalSymbolId is not null
                    || site.ResultStorageKind is not null) return false;
            }
        }
        return true;
    }

    private static bool ContainsReference(SemanticOperation operation, string symbolId) =>
        operation.Kind == "local_reference" && operation.SymbolId == symbolId
        || operation.Children?.Any(child => child is not null && ContainsReference(child, symbolId)) == true;

    private static bool IsWritableStaticResultField(SemanticDocument document,
        SemanticCallable callable, string fieldId, string? resultTypeId)
    {
        SemanticSymbol? methodSymbol = document.Symbols.FirstOrDefault(symbol =>
            symbol?.Id == callable.MethodSymbolId);
        return methodSymbol?.ContainingSymbolId is { } ownerId
            && document.Symbols.Any(symbol => symbol?.Id == fieldId
                && symbol.Kind == "field" && symbol.ContainingSymbolId == ownerId
                && symbol.TypeId == resultTypeId && symbol.IsStatic
                && !symbol.IsConst && !symbol.IsReadonly);
    }

    private static bool HasEarlierResultLocal(SemanticAsyncMethod method,
        SemanticAsyncAwaitSite site)
    {
        SemanticAsyncStateFlowAnalysis flow =
            SemanticAsyncStateFlowAnalyzer.AnalyzeControlFlow(method.Segments);
        return flow.Issues.Count == 0
            && flow.Locals.Any(local => local.SymbolId == site.ResultSymbolId
                && local.Span.Start < site.Span.Start);
    }

    public static bool HasLeadingTaskInitializers(SemanticAsyncMethod method,
        IReadOnlyCollection<string> localIds)
    {
        return TryGetTaskLocalBindings(method, localIds, false,
            out _, out _);
    }

    public static bool TryGetTaskLocalBindings(SemanticAsyncMethod method,
        IReadOnlyCollection<string> localIds, bool allowAliases,
        out IReadOnlyDictionary<string, string> producers,
        out IReadOnlyDictionary<string, string> aliases)
    {
        return TryGetTaskLocalFlow(method, localIds, allowAliases,
            out producers, out aliases, out _, out _);
    }

    public static bool TryGetTaskLocalFlow(SemanticAsyncMethod method,
        IReadOnlyCollection<string> localIds, bool allowAliases,
        out IReadOnlyDictionary<string, string> producers,
        out IReadOnlyDictionary<string, string> aliases,
        out IReadOnlyDictionary<int, IReadOnlyList<string>> activeAtEntry,
        out IReadOnlyDictionary<int, IReadOnlyList<string>> activeAtExit)
    {
        Dictionary<string, string> roots = new(StringComparer.Ordinal);
        Dictionary<string, string> sources = new(StringComparer.Ordinal);
        Dictionary<int, IReadOnlyList<string>> before = new();
        Dictionary<int, IReadOnlyList<string>> after = new();
        producers = roots;
        aliases = sources;
        activeAtEntry = before;
        activeAtExit = after;
        if (localIds.Count < 1 || localIds.Count > MaximumTaskLocalsPerMethod) return false;
        HashSet<string> owned = localIds.ToHashSet(StringComparer.Ordinal);
        if (owned.Count != localIds.Count) return false;
        Dictionary<int, SemanticAsyncSegment> segments = new();
        foreach (SemanticAsyncSegment segment in method.Segments)
        {
            if (!segments.TryAdd(segment.Ordinal, segment)) return false;
        }
        if (!segments.ContainsKey(method.EntrySegmentOrdinal)) return false;
        Dictionary<int, HashSet<string>> incoming = new()
        {
            [method.EntrySegmentOrdinal] = new(StringComparer.Ordinal),
        };
        Queue<int> pending = new();
        pending.Enqueue(method.EntrySegmentOrdinal);
        HashSet<string> declarations = new(StringComparer.Ordinal);
        List<(int Start, string Id)> declarationOrder = new(localIds.Count);
        while (pending.Count != 0)
        {
            int ordinal = pending.Dequeue();
            SemanticAsyncSegment segment = segments[ordinal];
            HashSet<string> active = new(incoming[ordinal], StringComparer.Ordinal);
            before.Add(ordinal, active.OrderBy(id => id, StringComparer.Ordinal).ToArray());
            foreach (SemanticAsyncStatement statement in segment.Statements)
            {
                if (statement.TargetSymbolId is not { } id || !owned.Contains(id))
                    continue;
                if (active.Contains(id) || !declarations.Add(id)) return false;
                if (statement.Operation is { Kind: "invocation", SymbolId: { } callableId })
                    roots.Add(id, callableId);
                else if (allowAliases && statement.Operation is
                    { Kind: "local_reference", SymbolId: { } sourceId, Children.Count: 0 }
                    && active.Contains(sourceId)
                    && roots.TryGetValue(sourceId, out string? rootId))
                {
                    roots.Add(id, rootId);
                    sources.Add(id, sourceId);
                }
                else return false;
                active.Add(id);
                declarationOrder.Add((statement.Operation.Span.Start, id));
            }
            if (segment.AwaitSite?.TaskLocalSymbolId is { } awaited
                && !active.Contains(awaited)) return false;
            after.Add(ordinal, active.OrderBy(id => id, StringComparer.Ordinal).ToArray());
            if (segment.Transfer is null) return false;
            int[] successors = segment.Transfer.Kind switch
            {
                SemanticAsyncMethod.GotoTransferKind or SemanticAsyncMethod.AwaitTransferKind =>
                    new[] { segment.Transfer.PrimaryTarget },
                SemanticAsyncMethod.BranchTransferKind =>
                    new[] { segment.Transfer.PrimaryTarget, segment.Transfer.SecondaryTarget },
                SemanticAsyncMethod.ReturnTransferKind or SemanticAsyncMethod.ThrowTransferKind =>
                    Array.Empty<int>(),
                _ => null!,
            };
            if (successors is null) return false;
            foreach (int successor in successors)
            {
                if (!segments.ContainsKey(successor)) return false;
                if (incoming.TryGetValue(successor, out HashSet<string>? established))
                {
                    // The Guest cannot guess which owners exist at a CFG join.
                    if (!established.SetEquals(active)) return false;
                }
                else
                {
                    incoming.Add(successor, new(active, StringComparer.Ordinal));
                    pending.Enqueue(successor);
                }
            }
        }
        return declarations.SetEquals(owned)
            && (method.TaskLocalSymbolIds is null
                || declarationOrder.OrderBy(item => item.Start)
                    .Select(item => item.Id).SequenceEqual(method.TaskLocalSymbolIds));
    }

    public static bool AllTaskLocalsReachAwait(IReadOnlyCollection<string> awaited,
        IReadOnlyCollection<string> owned,
        IReadOnlyDictionary<string, string> aliases)
    {
        HashSet<string> reachable = new(awaited, StringComparer.Ordinal);
        foreach (string id in awaited)
        {
            string cursor = id;
            while (aliases.TryGetValue(cursor, out string? source))
            {
                reachable.Add(source);
                cursor = source;
            }
        }
        return owned.All(reachable.Contains);
    }

    private static bool TaskLocalReferencesAreRestricted(SemanticAsyncMethod method,
        IReadOnlyCollection<string> owned)
    {
        HashSet<string> ids = owned.ToHashSet(StringComparer.Ordinal);
        foreach (SemanticAsyncSegment segment in method.Segments)
        {
            foreach (SemanticAsyncStatement statement in segment.Statements)
            {
                if (statement.Operation.Kind == "local_reference"
                    && statement.Operation.SymbolId is { } sourceId
                    && ids.Contains(sourceId) && statement.TargetSymbolId is { } targetId
                    && ids.Contains(targetId)) continue;
                if (ids.Any(id => ContainsReference(statement.Operation, id))) return false;
            }
            if (segment.Transfer?.Condition is { } condition
                && ids.Any(id => ContainsReference(condition, id))) return false;
        }
        return true;
    }

    private static bool IsSupportedTaskResult(
        SemanticDocument document, string returnTypeId, string resultTypeId)
    {
        return resultTypeId == "type:int32"
            && document.Types.Any(type => type?.Id == returnTypeId
                && type.CanonicalName == "global::System.Threading.Tasks.Task<int>")
            && document.TypeShapes.Any(shape => shape?.TypeId == returnTypeId
                && shape.GenericArgumentTypeIds is { Count: 1 }
                && shape.GenericArgumentTypeIds[0] == resultTypeId);
    }

    public static SemanticAsyncStateSlot[] MergeStateSlots(IReadOnlyList<SemanticAsyncStateSlot> locals,
        IReadOnlyList<SemanticAsyncStateSlot> inputs) => locals.Concat(inputs)
        .OrderBy(slot => slot.SymbolId, StringComparer.Ordinal).ToArray();
}
