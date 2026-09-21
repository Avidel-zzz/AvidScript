using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.CSharpSemantic;

public static class SemanticClosureContractValidator
{
    public static bool IsValid(SemanticDocument document)
    {
        if (document.ClosureEnvironments is null || document.ClosureBindings is null
            || !SemanticClosureAllocationValidator.IsValid(document)
            || !SemanticAsyncScopeValidator.IsValid(document)) return false;
        if (document.SchemaVersion < 22)
            return document.ClosureEnvironments.Count == 0 && document.ClosureBindings.Count == 0;
        if (document.Symbols is null || document.Callables is null || document.Methods is null
            || document.Symbols.Any(symbol => symbol is null || string.IsNullOrWhiteSpace(symbol.Id))
            || document.Callables.Any(callable => callable is null || string.IsNullOrWhiteSpace(callable.MethodSymbolId)
                || callable.Parameters is null || callable.Parameters.Any(parameter => parameter is null))
            || document.Symbols.Select(symbol => symbol.Id).Distinct(StringComparer.Ordinal).Count() != document.Symbols.Count
            || document.Callables.Select(callable => callable.MethodSymbolId).Distinct(StringComparer.Ordinal).Count() != document.Callables.Count)
            return false;
        Dictionary<string, SemanticSymbol> symbols = document.Symbols.ToDictionary(symbol => symbol.Id, StringComparer.Ordinal);
        Dictionary<string, SemanticCallable> callables = document.Callables.ToDictionary(callable => callable.MethodSymbolId, StringComparer.Ordinal);
        Dictionary<string, SemanticClosureCell> cells = new(StringComparer.Ordinal);
        HashSet<string> environments = new(StringComparer.Ordinal);
        foreach (SemanticClosureEnvironment environment in document.ClosureEnvironments)
        {
            if (environment is null || string.IsNullOrWhiteSpace(environment.OwnerMethodSymbolId)
                || !callables.TryGetValue(environment.OwnerMethodSymbolId, out SemanticCallable? owner)
                || environment.ScopeKind is not ("activation" or "block_entry" or "for_entry" or "foreach_iteration")
                || environment.ScopeOrdinal < 0 || (environment.ScopeKind == "activation" && environment.ScopeOrdinal != 0)
                || environment.Id != SemanticClosureEnvironment.GetId(environment.OwnerMethodSymbolId, environment.ScopeKind, environment.ScopeOrdinal)
                || !environments.Add(environment.Id) || environment.Span is null || environment.Span.Start < 0 || environment.Span.Length <= 0
                || environment.Cells is null || environment.Cells.Count == 0) return false;
            foreach (SemanticClosureCell cell in environment.Cells)
            {
                if (cell is null || string.IsNullOrWhiteSpace(cell.SymbolId) || string.IsNullOrWhiteSpace(cell.TypeId)
                    || !cells.TryAdd(cell.SymbolId, cell)) return false;
                if (cell.Kind == "receiver")
                {
                    if (cell.SymbolId != "receiver:" + owner.MethodSymbolId || cell.TypeId != owner.ContainingTypeId
                        || owner.IsStatic || environment.ScopeKind != "activation") return false;
                }
                else if (cell.Kind is not ("local" or "parameter") || !symbols.TryGetValue(cell.SymbolId, out SemanticSymbol? symbol)
                    || symbol.Kind != cell.Kind || symbol.TypeId != cell.TypeId || symbol.ContainingSymbolId != owner.MethodSymbolId
                    || symbol.IsConst || (cell.Kind == "parameter" && environment.ScopeKind != "activation")) return false;
            }
        }
        HashSet<string> targets = new(StringComparer.Ordinal);
        foreach (SemanticMethodBody method in document.Methods)
            if (method is null || !CollectTargets(method.Root, targets)) return false;
        Dictionary<string, SemanticClosureBinding> bindings = new(StringComparer.Ordinal);
        HashSet<string> requiredCells = new(StringComparer.Ordinal);
        foreach (SemanticClosureBinding binding in document.ClosureBindings)
        {
            if (binding is null || string.IsNullOrWhiteSpace(binding.MethodSymbolId)
                || !callables.ContainsKey(binding.MethodSymbolId) || !bindings.TryAdd(binding.MethodSymbolId, binding)
                || binding.IsDelegateTarget != targets.Contains(binding.MethodSymbolId)
                || binding.CellSymbolIds is null || binding.CellSymbolIds.Count == 0
                || binding.CellSymbolIds.Any(id => id is null || !cells.ContainsKey(id))
                || binding.CellSymbolIds.Distinct(StringComparer.Ordinal).Count() != binding.CellSymbolIds.Count) return false;
        }
        foreach (SemanticCallable callable in document.Callables)
        {
            string prefix = $"symbol:parameter:{callable.MethodSymbolId}:capture:";
            SemanticCallableParameter[] captured = callable.Parameters.Where(parameter =>
                parameter.SymbolId?.StartsWith(prefix, StringComparison.Ordinal) == true).ToArray();
            if (targets.Contains(callable.MethodSymbolId))
                foreach (SemanticCallableParameter parameter in captured) requiredCells.Add(parameter.SymbolId[prefix.Length..]);
            string[] shared = captured.Select(parameter => parameter.SymbolId[prefix.Length..]).Where(cells.ContainsKey)
                .OrderBy(id => id, StringComparer.Ordinal).ToArray();
            bindings.TryGetValue(callable.MethodSymbolId, out SemanticClosureBinding? binding);
            if (shared.Length == 0 ? binding is not null : binding is null
                || !shared.SequenceEqual(binding.CellSymbolIds.OrderBy(id => id, StringComparer.Ordinal))) return false;
            foreach (SemanticCallableParameter parameter in captured)
                if (cells.TryGetValue(parameter.SymbolId[prefix.Length..], out SemanticClosureCell? cell)
                    && (parameter.TypeId != cell.TypeId || parameter.RefKind != (cell.Kind == "receiver" ? "none" : "ref"))) return false;
        }
        return requiredCells.SetEquals(cells.Keys);
    }

    private static bool CollectTargets(SemanticOperation? operation, HashSet<string> targets)
    {
        if (operation is null || operation.Children is null) return false;
        if (operation.Kind == "method_reference" && operation.SymbolId is { } id) targets.Add(id);
        return operation.Children.All(child => CollectTargets(child, targets));
    }
}
