using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.CSharpSemantic;

public sealed record SemanticAsyncTaskLocalWrite(int StatementIndex, string SymbolId,
    SemanticOperation? Value, bool IsDeclaration);

public sealed record SemanticAsyncTaskLocalFlow(SemanticAsyncTaskOwnerStates States,
    IReadOnlyDictionary<int, IReadOnlyList<SemanticAsyncTaskLocalWrite>> WritesBySegment,
    IReadOnlyDictionary<int, IReadOnlyList<string>> ReleasedOnExit);

public static class SemanticAsyncTaskLocalLifetimeValidator
{
    public static bool IsValid(SemanticDocument document)
    {
        if (document?.AsyncMethods is null || document.Symbols is null || document.Types is null
            || document.Callables is null) return false;
        bool enabled = SemanticContract.HasTaskLocalLifetimes(document);
        if (!enabled) return document.SchemaVersion != SemanticContract.TaskLocalLifetimeSchemaVersion
            && document.SemanticVersion != SemanticContract.TaskLocalLifetimeSemanticVersion
            && document.SchemaVersion != SemanticContract.AsyncThrowRoutingSchemaVersion
            && document.SemanticVersion != SemanticContract.AsyncThrowRoutingSemanticVersion
            && document.SchemaVersion != SemanticContract.AsyncMemberAssignmentSchemaVersion
            && document.SemanticVersion != SemanticContract.AsyncMemberAssignmentSemanticVersion
            && document.AsyncMethods.All(method => method is not null && method.TaskLocalLifetimes is null);
        if (!SemanticContract.HasAsyncThrowRouting(document)
                && !document.AsyncMethods.Any(method => method?.TaskLocalLifetimes is not null)
            || !SemanticAsyncScopeValidator.IsValid(document)) return false;
        var taskTypes = document.Types.Where(type => type?.CanonicalName == "global::System.Threading.Tasks.Task<int>")
            .Select(type => type.Id).ToHashSet(StringComparer.Ordinal);
        bool ContainsTaskValue(SemanticOperation operation) => operation.TypeId is { } type && taskTypes.Contains(type)
            || operation.Children.Any(ContainsTaskValue);
        foreach (var method in document.AsyncMethods)
        {
            if (method is null) return false;
            var locals = document.Symbols.Where(symbol => symbol?.Kind == "local"
                && symbol.ContainingSymbolId == method.MethodSymbolId
                && symbol.TypeId is { } type && taskTypes.Contains(type)).OrderBy(symbol => symbol.Span.Start).ToArray();
            if (method.TaskLocalLifetimes is null)
            {
                if (locals.Length != 0) return false;
                continue;
            }
            if (!TryAnalyze(method, out var flow)) return false;
            if (!locals.Select(local => local.Id).SequenceEqual(method.TaskLocalSymbolIds!)) return false;
            var localTypes = locals.ToDictionary(local => local.Id, local => local.TypeId!, StringComparer.Ordinal);
            foreach (var local in locals)
            {
                // The binding is source-produced; readers also reject reparenting
                // to a wider scope or omitting a referenced lexical membership.
                var closest = method.LexicalScopes.Where(scope => scope.Kind is "block_entry" or "activation"
                    && Contains(scope.Span, local.Span)).OrderBy(scope => scope.Span.Length).FirstOrDefault();
                if (closest?.Id != method.TaskLocalLifetimes.Single(item => item.SymbolId == local.Id).ScopeId)
                    return false;
            }
            foreach (var write in flow!.WritesBySegment.Values.SelectMany(writes => writes))
            {
                if (write.Value is null) continue;
                if (write.Value.TypeId != localTypes[write.SymbolId]) return false;
                if (write.Value.Kind == "local_reference")
                {
                    if (!localTypes.TryGetValue(write.Value.SymbolId!, out var type)
                        || type != write.Value.TypeId) return false;
                    continue;
                }
                var targets = document.Callables.Where(callable => callable?.MethodSymbolId == write.Value.SymbolId).ToArray();
                if (targets.Length != 1) return false;
                var target = targets[0];
                if (!target.IsStatic || !target.HasBody || target.Import is not null || target.Export is not null
                    || target.IsConstructor || target.ReturnTypeId != write.Value.TypeId
                    || target.Parameters is null || !document.AsyncMethods.Any(producer =>
                        producer.MethodSymbolId == target.MethodSymbolId && producer.TaskResultTypeId == "type:int32")
                    || write.Value.Children.Count != target.Parameters.Count
                    || write.Value.Children.Any(ContainsTaskValue)) return false;
                for (int index = 0; index < target.Parameters.Count; index++)
                {
                    var argument = write.Value.Children[index];
                    var parameter = target.Parameters[index];
                    if (parameter.RefKind != "none" || argument.Kind != "argument"
                        || argument.SymbolId != parameter.SymbolId || argument.Children.Count != 1
                        || argument.Children[0].TypeId != parameter.TypeId) return false;
                }
            }
            foreach (var segment in method.Segments)
            {
                if (flow.WritesBySegment[segment.Ordinal].Where(write => write.Value is null)
                    .Any(write => segment.Statements[write.StatementIndex].Operation.TypeId != localTypes[write.SymbolId])) return false;
                var writeIndices = flow.WritesBySegment[segment.Ordinal].Select(write => write.StatementIndex).ToHashSet();
                if (segment.Statements.Where((_, index) => !writeIndices.Contains(index))
                        .Any(statement => ContainsTaskValue(statement.Operation))
                    || segment.Transfer?.Condition is { } condition && ContainsTaskValue(condition)
                    || segment.AwaitSite is { ProducerKind: not "task_local" } other
                        && other.Arguments.Any(ContainsTaskValue)) return false;
                if (segment.AwaitSite is not { ProducerKind: "task_local" } site) continue;
                if (site.TaskCallableId is not null || site.TaskLocalSymbolId is not { } id
                    || !localTypes.TryGetValue(id, out var type) || site.Arguments.Count != 1
                    || site.Arguments[0].TypeId != type || site.ResultTypeId != "type:int32") return false;
            }
        }
        return true;
    }

    public static bool TryAnalyze(SemanticAsyncMethod method, out SemanticAsyncTaskLocalFlow? flow)
    {
        flow = null;
        if (method?.TaskLocalSymbolIds is not { Count: > 0 and <= SemanticAsyncInvocationValidator.MaximumTaskLocalsPerMethod } ids
            || method.TaskLocalLifetimes is not { } lifetimes || lifetimes.Count != ids.Count
            || ids.Any(string.IsNullOrWhiteSpace) || ids.Distinct(StringComparer.Ordinal).Count() != ids.Count
            || lifetimes.Any(item => item is null || string.IsNullOrWhiteSpace(item.ScopeId))
            || !lifetimes.Select(item => item.SymbolId).SequenceEqual(ids)
            || method.LexicalScopes is null || method.LexicalScopes.Any(scope => scope?.Segments is null || scope.Span is null)
            || method.LexicalScopes.Select(scope => scope.Id).Distinct().Count() != method.LexicalScopes.Count
            || method.Lowering != SemanticAsyncMethod.ContinuationCfgLowering
            || method.Segments is not { Count: > 0 and <= SemanticAsyncMethod.MaximumControlFlowSegments }
            || method.Segments.Any(segment => segment?.Statements is null || segment.Transfer is null)) return false;
        var owned = ids.ToHashSet(StringComparer.Ordinal);
        var scopes = method.LexicalScopes.ToDictionary(scope => scope.Id, StringComparer.Ordinal);
        if (lifetimes.Any(item => !scopes.TryGetValue(item.ScopeId, out var scope)
            || scope.Kind is not ("block_entry" or "activation"))) return false;
        var memberships = lifetimes.ToDictionary(item => item.SymbolId,
            item => scopes[item.ScopeId].Segments.ToHashSet(), StringComparer.Ordinal);
        Dictionary<string, SemanticSpan> declarations = new(StringComparer.Ordinal);
        Dictionary<int, IReadOnlyList<SemanticAsyncTaskLocalWrite>> writesBySegment = new();
        List<SemanticAsyncTaskOwnerBlock> blocks = new();
        Dictionary<SemanticAsyncTaskOwnerEdge, IReadOnlyList<string>> releases = new();
        foreach (var segment in method.Segments)
        {
            List<SemanticAsyncTaskLocalWrite> writes = new();
            for (int index = 0; index < segment.Statements.Count; index++)
            {
                var statement = segment.Statements[index];
                if (statement?.Operation is not { } operation || !WellFormed(operation)) return false;
                var value = operation;
                string? target = null;
                bool declaration = false;
                if (statement.TargetSymbolId is { } declared && owned.Contains(declared))
                {
                    target = declared;
                    declaration = true;
                }
                else if (operation.Kind == SemanticAsyncMethod.LocalDeclarationOperationKind
                    && operation.SymbolId is { } uninitialized && owned.Contains(uninitialized))
                {
                    if (statement.TargetSymbolId is not null || operation.Children.Count != 0) return false;
                    target = uninitialized;
                    value = null;
                    declaration = true;
                }
                else if (statement.TargetSymbolId is null && operation is
                    { Kind: "expression_statement", Children.Count: 1 }
                    && operation.Children[0] is { Kind: "assignment", Children.Count: 2 } assignment
                    && assignment.Children[0] is { Kind: "local_reference", SymbolId: { } assigned, Children.Count: 0 }
                    && owned.Contains(assigned))
                {
                    target = assigned;
                    value = assignment.Children[1];
                    if (assignment.TypeId != value.TypeId || assignment.Children[0].TypeId != value.TypeId) return false;
                }
                if (target is null)
                {
                    if (ReferencesOwner(operation, owned)) return false;
                    continue;
                }
                if (!memberships[target].Contains(segment.Ordinal)
                    || declaration && !declarations.TryAdd(target, operation.Span)) return false;
                if (value is not null && (value.Kind == "local_reference"
                    ? value.SymbolId is not { } source || !owned.Contains(source) || value.Children.Count != 0
                    : value.Kind != "invocation" || string.IsNullOrWhiteSpace(value.SymbolId)
                        || value.Children.Any(child => ReferencesOwner(child, owned)))) return false;
                if (value?.Kind == "local_reference" && !memberships[value.SymbolId!].Contains(segment.Ordinal)) return false;
                writes.Add(new(index, target, value, declaration));
            }
            if (segment.Transfer!.Condition is { } condition
                && (!WellFormed(condition) || ReferencesOwner(condition, owned))) return false;
            if (segment.AwaitSite is { } site)
            {
                if (site.Arguments is null || site.Arguments.Any(argument => !WellFormed(argument))) return false;
                if (site.ProducerKind == "task_local")
                {
                    if (site.TaskLocalSymbolId is not { } awaited || !owned.Contains(awaited)
                        || site.TaskCallableId is not null || !memberships[awaited].Contains(segment.Ordinal)
                        || site.Arguments.Count != 1 || site.Arguments[0] is not
                            { Kind: "local_reference", Children.Count: 0 } reference || reference.SymbolId != awaited) return false;
                }
                else if (site.Arguments.Any(argument => ReferencesOwner(argument, owned))) return false;
                if (site.CancellationToken is { } token && (!WellFormed(token) || ReferencesOwner(token, owned))) return false;
            }
            if (!writesBySegment.TryAdd(segment.Ordinal, writes)) return false;
            int[] targets = SemanticAsyncScopeValidator.Targets(segment.Transfer).Distinct().ToArray();
            blocks.Add(new(segment.Ordinal, writes.Where(write => write.Value is not null)
                .Select(write => write.SymbolId).Distinct(StringComparer.Ordinal).ToArray(), targets));
            foreach (int target in targets)
                releases.Add(new(segment.Ordinal, target), ids.Where(id => memberships[id].Contains(segment.Ordinal)
                    && !memberships[id].Contains(target)).ToArray());
        }
        if (!declarations.OrderBy(pair => pair.Value.Start).Select(pair => pair.Key).SequenceEqual(ids)
            || !SemanticAsyncTaskOwnerFlowSolver.TrySolve(method.EntrySegmentOrdinal, ids, blocks, releases, out var states)) return false;
        foreach (var segment in method.Segments)
        {
            if (!states!.DefiniteAtEntry.TryGetValue(segment.Ordinal, out var definite)) continue;
            HashSet<string> available = new(definite, StringComparer.Ordinal);
            HashSet<string> possible = new(states.PossibleAtEntry[segment.Ordinal], StringComparer.Ordinal);
            foreach (var write in writesBySegment[segment.Ordinal])
            {
                if (write.IsDeclaration && possible.Contains(write.SymbolId)
                    || write.Value is { Kind: "local_reference", SymbolId: { } source } && !available.Contains(source)) return false;
                if (write.Value is not null) { available.Add(write.SymbolId); possible.Add(write.SymbolId); }
            }
            if (segment.AwaitSite?.TaskLocalSymbolId is { } awaited && !available.Contains(awaited)) return false;
        }
        flow = new(states!, writesBySegment, blocks.Where(block => block.Successors.Count == 0
            && states!.PossibleAtExit.ContainsKey(block.Ordinal)).ToDictionary(block => block.Ordinal,
                block => states!.PossibleAtExit[block.Ordinal]));
        return true;
    }

    private static bool WellFormed(SemanticOperation? operation, int depth = 0) => operation is not null
        && depth <= SemanticAsyncMethod.MaximumStructuredFlowNodes && operation.IsSupported
        && operation.Span is not null && operation.Children is not null
        && operation.Children.All(child => WellFormed(child, depth + 1));

    private static bool ReferencesOwner(SemanticOperation operation, HashSet<string> owned) =>
        operation.SymbolId is { } id && owned.Contains(id) || operation.Children.Any(child => ReferencesOwner(child, owned));

    private static bool Contains(SemanticSpan scope, SemanticSpan span) => span is not null
        && span.Start >= scope.Start && (long)span.Start + span.Length <= (long)scope.Start + scope.Length;
}
