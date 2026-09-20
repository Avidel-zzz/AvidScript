using System;
using System.Collections.Generic;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using AvidScript.CSharpSemantic;

namespace AvidScript.CSharpGuest;

internal sealed record CSharpUeDispatchTarget(uint TypeOrdinal, SemanticUeMethodEntry Method);
internal sealed record CSharpUeDispatchRoute(string Id, string Hash, SemanticCallable Callable,
    IReadOnlyList<CSharpUeDispatchTarget> Targets);

internal static class CSharpUeDispatch
{
    public static bool IsInterfaceView(SemanticDocument document, string typeId) =>
        document.UeMethodCatalog?.Interfaces.Any(contract => contract.TypeId == typeId) == true;

    public static bool TryRoute(SemanticDocument document, SemanticOperation operation, out CSharpUeDispatchRoute route)
    {
        route = null!;
        if (operation.Kind is not ("invocation" or "method_reference") || operation.Dispatch?.Kind is not ("virtual" or "interface")
            || document.UeMethodCatalog is not { } catalog || operation.SymbolId is null) return false;
        SemanticUeMethodEntry? declaration = catalog.Methods.FirstOrDefault(method => method.MethodSymbolId == operation.SymbolId);
        SemanticCallable? callable = document.Callables.FirstOrDefault(method => method.MethodSymbolId == operation.SymbolId);
        if (declaration is null || callable is null || callable.IsStatic || declaration.ReturnRefKind != "none") return false;
        List<CSharpUeDispatchTarget> targets = new();
        for (int ordinal = 0; ordinal < document.UeTypeDeclarations.Count; ++ordinal)
        {
            string typeId = document.UeTypeDeclarations[ordinal].TypeId;
            SemanticUeMethodType type = catalog.Types.Single(type => type.TypeId == typeId);
            string? implementation = operation.Dispatch.Kind == "virtual"
                ? type.VirtualSlots.FirstOrDefault(slot => slot.SlotMethodSymbolId == operation.Dispatch.SlotMethodSymbolId)?.ImplementationMethodSymbolId
                : type.InterfaceRoutes.FirstOrDefault(item => item.InterfaceMethodSymbolId == declaration.MethodSymbolId)?.ImplementationMethodSymbolId;
            SemanticUeMethodEntry? method = catalog.Methods.FirstOrDefault(method => method.MethodSymbolId == implementation);
            if (operation.Dispatch.Kind == "interface" && method?.Dispatch.SlotMethodSymbolId is { } slotId)
            {
                // Interface mapping identifies the implementing declaration. A
                // virtual implementation still dispatches through the target type's slot.
                string? actual = type.VirtualSlots.FirstOrDefault(slot => slot.SlotMethodSymbolId == slotId)?.ImplementationMethodSymbolId;
                method = catalog.Methods.FirstOrDefault(candidate => candidate.MethodSymbolId == actual);
            }
            if (method?.HasGuestBody == true) targets.Add(new((uint)ordinal, method));
        }
        string hash = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(
            operation.Dispatch.Kind + "\n" + declaration.MethodSymbolId + "\n" + operation.Dispatch.SlotMethodSymbolId))).ToLowerInvariant();
        route = new("$ue:dispatch:" + hash, hash, callable, targets);
        return true;
    }

    public static IReadOnlyList<CSharpUeDispatchRoute> Routes(SemanticDocument document)
    {
        var reachable = document.Reachability!.ReachableCallableIds.ToHashSet(StringComparer.Ordinal);
        return document.ControlFlowGraphs.Where(graph => reachable.Contains(graph.MethodSymbolId))
            .SelectMany(graph => graph.Blocks.Where(block => block.IsReachable))
            .SelectMany(block => block.BranchValue is null ? block.Operations : block.Operations.Append(block.BranchValue))
            .SelectMany(Operations).Select(operation => TryRoute(document, operation, out var route) ? route : null)
            .Where(route => route is not null).Cast<CSharpUeDispatchRoute>()
            .DistinctBy(route => route.Id).OrderBy(route => route.Id, StringComparer.Ordinal).ToArray();
    }

    public static SemanticDocument ExpandReachability(SemanticDocument document)
    {
        if (document.UeMethodCatalog is null || document.Reachability is null
            || document.Reachability.Mode == "all_callables_compatibility") return document;
        HashSet<string> additional = new(StringComparer.Ordinal);
        while (true)
        {
            int before = additional.Count;
            foreach (var target in Routes(document).SelectMany(route => route.Targets)) additional.Add(target.Method.MethodSymbolId);
            if (additional.Count == before) return document;
            document = document with { Reachability = SemanticReachability.ExpandForExecution(document,
                additional.OrderBy(id => id, StringComparer.Ordinal).ToArray()) };
        }
    }

    private static IEnumerable<SemanticOperation> Operations(SemanticOperation operation)
    {
        yield return operation;
        foreach (var child in operation.Children.SelectMany(Operations)) yield return child;
    }
}
