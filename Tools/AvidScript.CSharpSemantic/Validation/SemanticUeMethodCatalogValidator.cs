using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.CSharpSemantic;

public static class SemanticUeMethodCatalogValidator
{
    public static bool IsValid(SemanticDocument document)
    {
        if (document.SchemaVersion < 26) return document.UeMethodCatalog is null;
        if (!((document.SchemaVersion == 26 && document.SemanticVersion == "1.30")
                || (document.SchemaVersion == 27 && document.SemanticVersion == "1.31")
                || (document.SchemaVersion == 28 && document.SemanticVersion == "1.32")
                || (document.SchemaVersion == SemanticContract.CurrentSchemaVersion && document.SemanticVersion == SemanticContract.CurrentSemanticVersion))
            || document.UeMethodCatalog is not { SchemaVersion: 1 } catalog
            || !Sorted(catalog.Methods, item => item.MethodSymbolId)
            || !Sorted(catalog.Types, item => item.TypeId) || !Sorted(catalog.Interfaces, item => item.TypeId)
            || !SemanticClassContractValidator.IsValid(document) || !SemanticDispatchContractValidator.IsValid(document)
            || document.UeTypeDeclarations is null || document.UeTypeDeclarations.Any(item => item is null || item.Functions is null)) return false;
        var types = document.Types.ToDictionary(item => item.Id, StringComparer.Ordinal);
        var classes = document.ClassTypes.ToDictionary(item => item.TypeId, StringComparer.Ordinal);
        var callables = document.Callables.ToDictionary(item => item.MethodSymbolId, StringComparer.Ordinal);
        var methods = catalog.Methods.ToDictionary(item => item.MethodSymbolId, StringComparer.Ordinal);
        var interfaces = catalog.Interfaces.ToDictionary(item => item.TypeId, StringComparer.Ordinal);
        var reflected = document.UeTypeDeclarations.SelectMany(item => item.Functions).Select(item => item.MethodSymbolId).ToHashSet(StringComparer.Ordinal);
        if (!catalog.Types.Select(item => item.TypeId).SequenceEqual(document.UeTypeDeclarations.Select(item => item.TypeId).OrderBy(id => id, StringComparer.Ordinal))) return false;
        foreach (SemanticUeMethodEntry method in catalog.Methods)
        {
            if (string.IsNullOrWhiteSpace(method.DeclarationMethodSymbolId) || string.IsNullOrWhiteSpace(method.Name)
                || method.MethodSymbolId != method.DeclarationMethodSymbolId
                    && method.MethodSymbolId != SemanticUeMethodEntry.GetConstructedId(method.DeclarationMethodSymbolId, method.ContainingTypeId)
                || method.ContainingTypeId is null || !types.ContainsKey(method.ContainingTypeId)
                || method.ReturnTypeId is null || !types.ContainsKey(method.ReturnTypeId)
                || method.ReturnRefKind is not ("none" or "ref" or "ref_readonly")
                || method.ReturnTypeId == "type:void" && method.ReturnRefKind != "none"
                || method.Parameters is null || method.Parameters.Any(parameter => parameter is null
                    || parameter.TypeId is null || parameter.TypeId == "type:void" || !types.ContainsKey(parameter.TypeId)
                    || parameter.RefKind is not ("none" or "ref" or "out" or "in" or "ref_readonly"))
                || !method.Parameters.Select(item => item.Ordinal).SequenceEqual(Enumerable.Range(0, method.Parameters.Count))
                || method.GenericArity < 0 || method.Accessibility is not ("public" or "private" or "protected" or "internal" or "protectedorinternal" or "protectedandinternal")
                || method.Dispatch is null
                || method.SignatureId != SemanticUeMethodEntry.GetSignatureId(method.ReturnTypeId, method.ReturnRefKind, method.GenericArity, method.Parameters)
                || method.IsReflected != reflected.Contains(method.MethodSymbolId)) return false;
            bool known = callables.TryGetValue(method.MethodSymbolId, out SemanticCallable? callable);
            if (method.HasGuestBody != (known && callable!.HasBody)) return false;
            if (document.Symbols?.FirstOrDefault(item => item.Id == method.DeclarationMethodSymbolId) is { } symbol && symbol.Name != method.Name) return false;
            if (known && (callable!.IsStatic || callable.IsConstructor || callable.ContainingTypeId != method.ContainingTypeId
                || callable.ReturnTypeId != method.ReturnTypeId || !SameDispatch(callable.Dispatch!, method.Dispatch, methods)
                || !callable.Parameters.Select(item => (item.TypeId, item.RefKind)).SequenceEqual(method.Parameters.Select(item => (item.TypeId, item.RefKind))))) return false;
        }
        // Reuse declaration/slot validation for external base and interface methods too.
        var expanded = catalog.Methods.Select(item =>
            new SemanticCallable(item.MethodSymbolId, item.ContainingTypeId, item.ReturnTypeId,
                item.Parameters.Select(parameter => new SemanticCallableParameter(parameter.Ordinal, item.MethodSymbolId + ":parameter:" + parameter.Ordinal,
                    "p" + parameter.Ordinal, parameter.TypeId, parameter.RefKind)).ToArray(), false, false, item.HasGuestBody, null, null, null, null, item.Dispatch)).ToArray();
        if (!SemanticDispatchContractValidator.IsValid(document with { Callables = expanded, Methods = Array.Empty<SemanticMethodBody>(),
            ControlFlowGraphs = Array.Empty<SemanticControlFlowGraph>(), AsyncMethods = Array.Empty<SemanticAsyncMethod>() })) return false;
        foreach (SemanticUeInterfaceContract contract in catalog.Interfaces)
        {
            if (!types.TryGetValue(contract.TypeId, out SemanticType? type) || type.Kind != "interface"
                || !Sorted(contract.BaseInterfaceTypeIds) || contract.BaseInterfaceTypeIds.Any(id => id == contract.TypeId || !interfaces.ContainsKey(id))
                || !Sorted(contract.MethodIds) || !contract.MethodIds.SequenceEqual(catalog.Methods.Where(item => item.ContainingTypeId == contract.TypeId).Select(item => item.MethodSymbolId))) return false;
        }
        foreach (string id in interfaces.Keys) if (InterfaceClosure(new[] { id }) is null) return false;
        foreach (SemanticUeMethodType type in catalog.Types)
        {
            if (!classes.TryGetValue(type.TypeId, out SemanticClassType? owner)
                || !Sorted(type.DeclaredMethodIds) || !type.DeclaredMethodIds.SequenceEqual(catalog.Methods.Where(item => item.ContainingTypeId == type.TypeId).Select(item => item.MethodSymbolId))
                || !Sorted(type.InterfaceTypeIds) || type.InterfaceTypeIds.Any(id => !interfaces.ContainsKey(id))
                || !Sorted(type.VirtualSlots, item => item.SlotMethodSymbolId)
                || !Sorted(type.InterfaceRoutes, item => item.InterfaceMethodSymbolId)) return false;
            foreach (SemanticCallable callable in document.Callables.Where(item => item.ContainingTypeId == type.TypeId && !item.IsStatic && !item.IsConstructor))
                if (!methods.ContainsKey(callable.MethodSymbolId)) return false;
            List<string> hierarchy = new();
            for (SemanticClassType? current = owner; current is not null; current = current.BaseTypeId is null ? null : classes[current.BaseTypeId]) hierarchy.Add(current.TypeId);
            HashSet<string>? allInterfaces = InterfaceClosure(hierarchy.SelectMany(id => classes[id].InterfaceTypeIds));
            if (allInterfaces is null || !type.InterfaceTypeIds.SequenceEqual(allInterfaces.OrderBy(id => id, StringComparer.Ordinal))) return false;
            Dictionary<string, string> slots = new(StringComparer.Ordinal);
            foreach (string ancestor in hierarchy.AsEnumerable().Reverse())
                foreach (SemanticUeMethodEntry method in catalog.Methods.Where(item => item.ContainingTypeId == ancestor))
                    if (method.Dispatch.SlotMethodSymbolId is { } slot) slots[slot] = method.MethodSymbolId;
            if (!type.VirtualSlots.Select(item => (item.SlotMethodSymbolId, item.ImplementationMethodSymbolId))
                .SequenceEqual(slots.OrderBy(pair => pair.Key, StringComparer.Ordinal).Select(pair => (pair.Key, pair.Value)))) return false;
            if (!type.InterfaceRoutes.Select(item => item.InterfaceMethodSymbolId).SequenceEqual(type.InterfaceTypeIds
                .SelectMany(id => interfaces[id].MethodIds).OrderBy(id => id, StringComparer.Ordinal))) return false;
            foreach (SemanticUeInterfaceRoute route in type.InterfaceRoutes)
            {
                if (!methods.TryGetValue(route.InterfaceMethodSymbolId, out SemanticUeMethodEntry? declaration)) return false;
                if (route.Kind == "unresolved")
                {
                    if (route.ImplementationMethodSymbolId is not null || !owner.IsAbstract) return false;
                    continue;
                }
                if (route.ImplementationMethodSymbolId is null || !methods.TryGetValue(route.ImplementationMethodSymbolId, out SemanticUeMethodEntry? implementation)
                    || declaration.SignatureId != implementation.SignatureId
                    || (route.Kind == "default_interface"
                        ? !type.InterfaceTypeIds.Contains(implementation.ContainingTypeId) || implementation.Dispatch.IsAbstract
                        : !hierarchy.Contains(implementation.ContainingTypeId)
                            || route.Kind != (implementation.Dispatch.SlotMethodSymbolId is not null ? "virtual" : "direct"))) return false;
                if (implementation.Dispatch.ExplicitInterfaceMethodIds.Count != 0)
                {
                    if (!implementation.Dispatch.ExplicitInterfaceMethodIds.Contains(declaration.MethodSymbolId)) return false;
                }
                else if (implementation.Name != declaration.Name || implementation.Accessibility != "public") return false;
            }
        }
        return true;

        HashSet<string>? InterfaceClosure(IEnumerable<string> roots)
        {
            HashSet<string> complete = new(StringComparer.Ordinal), active = new(StringComparer.Ordinal);
            Stack<(string Id, bool Exit)> pending = new(roots.Select(id => (id, false)));
            while (pending.TryPop(out var current))
            {
                if (current.Exit) { active.Remove(current.Id); complete.Add(current.Id); continue; }
                if (complete.Contains(current.Id)) continue;
                if (!active.Add(current.Id) || !interfaces.TryGetValue(current.Id, out SemanticUeInterfaceContract? contract)
                    || contract.BaseInterfaceTypeIds is null) return null;
                pending.Push((current.Id, true));
                foreach (string parent in contract.BaseInterfaceTypeIds) pending.Push((parent, false));
            }
            return complete;
        }
    }

    private static bool Sorted(IReadOnlyList<string>? items) => items is not null && items.All(item => !string.IsNullOrWhiteSpace(item))
        && items.SequenceEqual(items.Distinct(StringComparer.Ordinal).OrderBy(item => item, StringComparer.Ordinal));
    private static bool Sorted<T>(IReadOnlyList<T>? items, Func<T, string> key) where T : class =>
        items is not null && items.All(item => item is not null) && Sorted(items.Select(key).ToArray());
    private static bool SameDispatch(SemanticCallableDispatch a, SemanticCallableDispatch b, IReadOnlyDictionary<string, SemanticUeMethodEntry> methods)
    {
        string? Declaration(string? id) => id is not null && methods.TryGetValue(id, out SemanticUeMethodEntry? entry) ? entry.DeclarationMethodSymbolId : id;
        return
        a.IsVirtual == b.IsVirtual && a.IsAbstract == b.IsAbstract && a.IsOverride == b.IsOverride && a.IsSealed == b.IsSealed
        && a.OverriddenMethodSymbolId == Declaration(b.OverriddenMethodSymbolId) && a.SlotMethodSymbolId == Declaration(b.SlotMethodSymbolId)
        && b.ExplicitInterfaceMethodIds is not null && a.ExplicitInterfaceMethodIds.SequenceEqual(b.ExplicitInterfaceMethodIds.Select(Declaration).Distinct(StringComparer.Ordinal).OrderBy(id => id, StringComparer.Ordinal));
    }
}
