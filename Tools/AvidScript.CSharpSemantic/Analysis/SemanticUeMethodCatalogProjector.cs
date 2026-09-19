using System;
using System.Collections.Generic;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace AvidScript.CSharpSemantic;

internal static class SemanticUeMethodCatalogProjector
{
    public static SemanticUeMethodCatalog Project(SemanticCompilationContext context,
        SemanticTypeRegistry registry, IReadOnlyList<SemanticUeTypeDeclaration> declarations,
        IReadOnlyList<SemanticCallable> callables)
    {
        if (declarations.Count == 0) return SemanticUeMethodCatalog.Empty;
        var wanted = declarations.Select(item => item.TypeId).ToHashSet(StringComparer.Ordinal);
        var reflected = declarations.SelectMany(item => item.Functions).Select(item => item.MethodSymbolId).ToHashSet(StringComparer.Ordinal);
        var bodies = callables.Where(item => item.HasBody).Select(item => item.MethodSymbolId).ToHashSet(StringComparer.Ordinal);
        Dictionary<string, INamedTypeSymbol> owners = new(StringComparer.Ordinal);
        foreach (SemanticCompilationUnit unit in context.ProjectionUnits)
        {
            SemanticModel model = context.Compilation.GetSemanticModel(unit.SyntaxTree);
            foreach (TypeDeclarationSyntax declaration in unit.SyntaxTree.GetRoot().DescendantNodes().OfType<TypeDeclarationSyntax>())
                if (model.GetDeclaredSymbol(declaration) is INamedTypeSymbol symbol)
                {
                    string id = "type:" + SemanticTypeRegistry.GetCanonicalName(symbol);
                    if (wanted.Contains(id)) owners.TryAdd(id, symbol);
                }
        }
        Dictionary<string, SemanticUeMethodEntry> methods = new(StringComparer.Ordinal);
        Dictionary<string, SemanticUeInterfaceContract> interfaces = new(StringComparer.Ordinal);
        List<SemanticUeMethodType> types = new();
        foreach ((string id, INamedTypeSymbol owner) in owners.OrderBy(pair => pair.Key, StringComparer.Ordinal))
        {
            Stack<INamedTypeSymbol> chain = new();
            for (INamedTypeSymbol? current = owner; current is not null; current = current.BaseType) chain.Push(current);
            Dictionary<string, string> slots = new(StringComparer.Ordinal);
            while (chain.TryPop(out INamedTypeSymbol? current))
                foreach (IMethodSymbol method in InstanceMethods(current))
                {
                    SemanticUeMethodEntry entry = Add(method);
                    if (entry.Dispatch.SlotMethodSymbolId is { } slot) slots[slot] = entry.MethodSymbolId;
                }
            List<SemanticUeInterfaceRoute> routes = new();
            foreach (INamedTypeSymbol contract in owner.AllInterfaces.OrderBy(SemanticTypeRegistry.GetCanonicalName, StringComparer.Ordinal))
            {
                string interfaceId = registry.Register(contract);
                IMethodSymbol[] members = InstanceMethods(contract).ToArray();
                interfaces.TryAdd(interfaceId, new(interfaceId,
                    contract.Interfaces.Select(registry.Register).OrderBy(value => value, StringComparer.Ordinal).ToArray(),
                    members.Select(method => Add(method).MethodSymbolId).OrderBy(value => value, StringComparer.Ordinal).ToArray()));
                foreach (IMethodSymbol member in members)
                {
                    IMethodSymbol? implementation = owner.FindImplementationForInterfaceMember(member) as IMethodSymbol;
                    SemanticUeMethodEntry? target = implementation is null ? null : Add(implementation);
                    routes.Add(new(Id(member), target?.MethodSymbolId,
                        target is null ? "unresolved" : implementation!.ContainingType.TypeKind == TypeKind.Interface
                            ? "default_interface" : target.Dispatch.SlotMethodSymbolId is not null ? "virtual" : "direct"));
                }
            }
            types.Add(new(id, InstanceMethods(owner).Select(method => Add(method).MethodSymbolId).OrderBy(value => value, StringComparer.Ordinal).ToArray(),
                slots.OrderBy(pair => pair.Key, StringComparer.Ordinal).Select(pair => new SemanticUeVirtualSlot(pair.Key, pair.Value)).ToArray(),
                owner.AllInterfaces.Select(registry.Register).OrderBy(value => value, StringComparer.Ordinal).ToArray(),
                routes.OrderBy(route => route.InterfaceMethodSymbolId, StringComparer.Ordinal).ToArray()));
        }
        return new(1, methods.Values.OrderBy(item => item.MethodSymbolId, StringComparer.Ordinal).ToArray(), types,
            interfaces.Values.OrderBy(item => item.TypeId, StringComparer.Ordinal).ToArray());

        SemanticUeMethodEntry Add(IMethodSymbol method)
        {
            string id = Id(method);
            if (methods.TryGetValue(id, out SemanticUeMethodEntry? existing)) return existing;
            string returnType = registry.Register(method.ReturnType);
            string returnRef = method.ReturnsByRefReadonly ? "ref_readonly" : method.ReturnsByRef ? "ref" : "none";
            SemanticUeMethodParameter[] parameters = method.Parameters.Select(parameter => new SemanticUeMethodParameter(parameter.Ordinal,
                registry.Register(parameter.Type), SemanticTypeRegistry.GetDelegateRefKind(parameter.RefKind))).ToArray();
            SemanticCallableDispatch dispatch = SemanticDispatchProjector.Project(method);
            IMethodSymbol slot = method;
            while (slot.OverriddenMethod is { } parent) slot = parent;
            dispatch = dispatch with {
                OverriddenMethodSymbolId = method.OverriddenMethod is { } overridden ? Id(overridden) : null,
                SlotMethodSymbolId = dispatch.SlotMethodSymbolId is null ? null : Id(slot),
                ExplicitInterfaceMethodIds = method.ExplicitInterfaceImplementations.Select(Id).Distinct(StringComparer.Ordinal).OrderBy(value => value, StringComparer.Ordinal).ToArray() };
            var entry = new SemanticUeMethodEntry(id, SemanticSymbolProjector.GetSymbolId(method), method.Name, registry.Register(method.ContainingType),
                SemanticUeMethodEntry.GetSignatureId(returnType, returnRef, method.Arity, parameters), returnType, returnRef, parameters,
                method.Arity, method.DeclaredAccessibility.ToString().ToLowerInvariant(), bodies.Contains(id), reflected.Contains(id),
                dispatch);
            methods.Add(id, entry);
            return entry;
        }
    }

    private static string Id(IMethodSymbol method) => SymbolEqualityComparer.Default.Equals(method.ContainingType, method.OriginalDefinition.ContainingType)
        ? SemanticSymbolProjector.GetSymbolId(method)
        : SemanticUeMethodEntry.GetConstructedId(SemanticSymbolProjector.GetSymbolId(method), "type:" + SemanticTypeRegistry.GetCanonicalName(method.ContainingType));

    private static IEnumerable<IMethodSymbol> InstanceMethods(INamedTypeSymbol type) => type.GetMembers()
        .SelectMany(member => member switch
        {
            IMethodSymbol method => new IMethodSymbol?[] { method },
            IPropertySymbol property => new[] { property.GetMethod, property.SetMethod },
            IEventSymbol eventSymbol => new[] { eventSymbol.AddMethod, eventSymbol.RemoveMethod },
            _ => Array.Empty<IMethodSymbol?>(),
        })
        .OfType<IMethodSymbol>().Where(method => !method.IsStatic && method.MethodKind is not (MethodKind.Constructor or MethodKind.Destructor))
        .GroupBy(Id, StringComparer.Ordinal).Select(group => group.First())
        .OrderBy(Id, StringComparer.Ordinal);
}
