using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.CSharpSemantic;

public static class SemanticDispatchContractValidator
{
    public static bool IsValid(SemanticDocument document)
    {
        if (document.Callables is null || document.Methods is null || document.ControlFlowGraphs is null
            || document.AsyncMethods is null || document.Types is null
            || document.Callables.Any(item => item is null || string.IsNullOrWhiteSpace(item.MethodSymbolId)
                || item.Parameters is null || item.Parameters.Any(parameter => parameter is null))
            || document.Callables.Select(item => item.MethodSymbolId).Distinct(StringComparer.Ordinal).Count() != document.Callables.Count)
            return false;
        bool current = document.SchemaVersion == 25 && document.SemanticVersion == "1.29"
            || document.SchemaVersion == 26 && document.SemanticVersion == "1.30"
            || document.SchemaVersion == 27 && document.SemanticVersion == "1.31"
            || document.SchemaVersion == 28 && document.SemanticVersion == "1.32"
            || document.SchemaVersion == 29 && document.SemanticVersion == "1.33"
            || document.SchemaVersion == SemanticContract.CurrentSchemaVersion && document.SemanticVersion == SemanticContract.CurrentSemanticVersion;
        if (document.SchemaVersion >= 25 && !current) return false;
        var callables = document.Callables.ToDictionary(item => item.MethodSymbolId, StringComparer.Ordinal);
        var interfaces = document.Types.Where(item => item is not null && item.Kind == "interface")
            .Select(item => item.Id).ToHashSet(StringComparer.Ordinal);
        var values = document.Types.Where(item => item is not null && item.IsValueType)
            .Select(item => item.Id).ToHashSet(StringComparer.Ordinal);
        foreach (SemanticCallable method in document.Callables)
        {
            SemanticCallableDispatch? dispatch = method.Dispatch;
            if (!current) { if (dispatch is not null) return false; continue; }
            if (dispatch is null || dispatch.ExplicitInterfaceMethodIds is null
                || dispatch.ExplicitInterfaceMethodIds.Any(string.IsNullOrWhiteSpace)
                || !dispatch.ExplicitInterfaceMethodIds.SequenceEqual(dispatch.ExplicitInterfaceMethodIds
                    .Distinct(StringComparer.Ordinal).OrderBy(id => id, StringComparer.Ordinal))
                || dispatch.IsOverride != (dispatch.OverriddenMethodSymbolId is not null)
                || dispatch.OverriddenMethodSymbolId is not null && string.IsNullOrWhiteSpace(dispatch.OverriddenMethodSymbolId)
                || dispatch.IsSealed && !dispatch.IsOverride && !interfaces.Contains(method.ContainingTypeId)
                || dispatch.IsAbstract && (method.HasBody || dispatch.IsSealed)
                || dispatch.IsVirtual && dispatch.IsOverride
                || (dispatch.IsVirtual || dispatch.IsAbstract || dispatch.IsOverride) != (dispatch.SlotMethodSymbolId is not null)
                || dispatch.SlotMethodSymbolId is not null && string.IsNullOrWhiteSpace(dispatch.SlotMethodSymbolId)
                || !dispatch.IsOverride && dispatch.SlotMethodSymbolId is not null && dispatch.SlotMethodSymbolId != method.MethodSymbolId
                || method.IsConstructor && (dispatch.SlotMethodSymbolId is not null || dispatch.ExplicitInterfaceMethodIds.Count != 0)
                || method.IsStatic && !interfaces.Contains(method.ContainingTypeId) && dispatch.SlotMethodSymbolId is not null)
                return false;
            if (dispatch.OverriddenMethodSymbolId is { } parentId && callables.TryGetValue(parentId, out SemanticCallable? parent))
            {
                if (parent.Dispatch is not { SlotMethodSymbolId: not null } parentDispatch || parentDispatch.IsSealed
                    || parent.IsStatic != method.IsStatic || parent.ContainingTypeId == method.ContainingTypeId
                    || parentDispatch.SlotMethodSymbolId != dispatch.SlotMethodSymbolId
                    || parent.Parameters is null || method.Parameters is null
                    || !parent.Parameters.Select(item => (item.TypeId, item.RefKind))
                        .SequenceEqual(method.Parameters.Select(item => (item.TypeId, item.RefKind)))) return false;
            }
            foreach (string interfaceId in dispatch.ExplicitInterfaceMethodIds)
                if (callables.TryGetValue(interfaceId, out SemanticCallable? member)
                    && (!interfaces.Contains(member.ContainingTypeId) || member.IsStatic != method.IsStatic
                        || member.ReturnTypeId != method.ReturnTypeId
                        || !member.Parameters.Select(item => (item.TypeId, item.RefKind))
                            .SequenceEqual(method.Parameters.Select(item => (item.TypeId, item.RefKind))))
                    && !HasConstructedInterfaceSignature(document, method, interfaceId)) return false;
        }
        // External base methods need not have executable bodies in this artifact.
        // Known chains must nevertheless terminate and agree on their slot identity.
        HashSet<string> complete = new(StringComparer.Ordinal);
        foreach (SemanticCallable method in document.Callables)
        {
            HashSet<string> path = new(StringComparer.Ordinal);
            SemanticCallable? node = method;
            while (node is not null && !complete.Contains(node.MethodSymbolId))
            {
                if (!path.Add(node.MethodSymbolId)) return false;
                node = node.Dispatch?.OverriddenMethodSymbolId is { } parentId
                    && callables.TryGetValue(parentId, out SemanticCallable? parent) ? parent : null;
            }
            complete.UnionWith(path);
        }
        var pending = new Stack<SemanticOperation>();
        foreach (SemanticMethodBody body in document.Methods)
        {
            if (body is null || body.Root is null) return false;
            pending.Push(body.Root);
        }
        foreach (SemanticControlFlowGraph graph in document.ControlFlowGraphs)
        {
            if (graph?.Blocks is null) return false;
            foreach (SemanticBasicBlock block in graph.Blocks)
            {
                if (block?.Operations is null) return false;
                foreach (SemanticOperation operation in block.Operations) pending.Push(operation);
                if (block.BranchValue is not null) pending.Push(block.BranchValue);
            }
        }
        foreach (SemanticAsyncMethod method in document.AsyncMethods)
        {
            if (method?.Segments is null) return false;
            foreach (SemanticAsyncSegment segment in method.Segments)
            {
                if (segment?.Statements is null) return false;
                foreach (SemanticAsyncStatement statement in segment.Statements)
                {
                    if (statement is null) return false;
                    pending.Push(statement.Operation);
                }
                if (segment.Transfer?.Condition is { } condition) pending.Push(condition);
                if (segment.AwaitSite is not { } site) continue;
                if (site.Arguments is null) return false;
                foreach (SemanticOperation argument in site.Arguments) pending.Push(argument);
                if (site.CancellationToken is not null) pending.Push(site.CancellationToken);
            }
        }
        while (pending.TryPop(out SemanticOperation? operation))
        {
            if (operation?.Children is null) return false;
            bool methodOperation = operation.Kind is "invocation" or "method_reference";
            if (!current || !methodOperation) { if (operation.Dispatch is not null) return false; }
            else
            {
                if (operation.Dispatch is not { } dispatch || string.IsNullOrWhiteSpace(operation.SymbolId)
                    || dispatch.Kind is not ("static" or "direct" or "virtual" or "interface" or "delegate")
                    || dispatch.IsBase && dispatch.Kind != "direct"
                    || dispatch.Kind == "virtual" && string.IsNullOrWhiteSpace(dispatch.SlotMethodSymbolId)
                    || dispatch.SlotMethodSymbolId is not null && string.IsNullOrWhiteSpace(dispatch.SlotMethodSymbolId)) return false;
                if (callables.TryGetValue(operation.SymbolId, out SemanticCallable? target)
                    && (target.Dispatch?.SlotMethodSymbolId != dispatch.SlotMethodSymbolId
                        || target.IsStatic && dispatch.Kind is not ("static" or "interface")
                        || !target.IsStatic && dispatch.Kind == "static"
                        || dispatch.Kind == "delegate"
                        || dispatch.Kind == "direct" && !dispatch.IsBase
                            && target.Dispatch?.SlotMethodSymbolId is not null && target.Dispatch.IsSealed == false
                            && !values.Contains(target.ContainingTypeId)
                        || interfaces.Contains(target.ContainingTypeId) != (dispatch.Kind == "interface"))) return false;
            }
            foreach (SemanticOperation child in operation.Children) pending.Push(child);
        }
        return true;
    }

    private static bool HasConstructedInterfaceSignature(SemanticDocument document, SemanticCallable implementation, string declarationId)
    {
        if (!((document.SchemaVersion == 26 && document.SemanticVersion == "1.30")
                || (document.SchemaVersion == 27 && document.SemanticVersion == "1.31")
                || (document.SchemaVersion == 28 && document.SemanticVersion == "1.32")
                || (document.SchemaVersion == 29 && document.SemanticVersion == "1.33")
                || (document.SchemaVersion == SemanticContract.CurrentSchemaVersion && document.SemanticVersion == SemanticContract.CurrentSemanticVersion)) || implementation.IsStatic
            || document.UeMethodCatalog?.Methods is not { } methods) return false;
        SemanticUeMethodEntry? owner = methods.FirstOrDefault(item => item is not null && item.MethodSymbolId == implementation.MethodSymbolId);
        if (owner?.Dispatch?.ExplicitInterfaceMethodIds is not { } targets) return false;
        return methods.Any(item => item is not null && item.DeclarationMethodSymbolId == declarationId
            && item.MethodSymbolId != declarationId && item.MethodSymbolId == SemanticUeMethodEntry.GetConstructedId(declarationId, item.ContainingTypeId)
            && targets.Contains(item.MethodSymbolId) && item.ReturnTypeId == implementation.ReturnTypeId
            && document.Types.Any(type => type.Id == item.ContainingTypeId && type.Kind == "interface")
            && item.Parameters is not null && item.Parameters.All(parameter => parameter is not null)
            && item.Parameters.Select(parameter => (parameter.TypeId, parameter.RefKind))
                .SequenceEqual(implementation.Parameters.Select(parameter => (parameter.TypeId, parameter.RefKind))));
    }
}
