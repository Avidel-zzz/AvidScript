using System;
using System.Linq;

namespace AvidScript.CSharpSemantic;

// Cold-path semantic selection only. A result does not authorize a Runtime entry.
public static class SemanticUeMethodResolver
{
    public static SemanticUeMethodEntry? Resolve(SemanticDocument document, string receiverTypeId,
        string methodSymbolId, string dispatchKind)
    {
        if (!SemanticUeMethodCatalogValidator.IsValid(document) || document.UeMethodCatalog is not { } catalog) return null;
        SemanticUeMethodType? receiver = catalog.Types.SingleOrDefault(item => item.TypeId == receiverTypeId);
        SemanticUeMethodEntry? method = catalog.Methods.SingleOrDefault(item => item.MethodSymbolId == methodSymbolId);
        if (receiver is null || method is null) return null;
        if (dispatchKind == "interface")
        {
            SemanticUeInterfaceRoute? route = receiver.InterfaceRoutes.SingleOrDefault(item => item.InterfaceMethodSymbolId == methodSymbolId);
            if (route?.ImplementationMethodSymbolId is null) return null;
            method = catalog.Methods.Single(item => item.MethodSymbolId == route.ImplementationMethodSymbolId);
            dispatchKind = route.Kind == "virtual" ? "virtual" : "direct";
            if (route.Kind == "default_interface") return method.Dispatch.IsAbstract ? null : method;
        }
        bool compatible = false;
        string? current = receiverTypeId;
        while (current is not null)
        {
            if (current == method.ContainingTypeId) { compatible = true; break; }
            current = document.ClassTypes.Single(item => item.TypeId == current).BaseTypeId;
        }
        if (!compatible) return null;
        if (dispatchKind == "virtual")
        {
            SemanticUeVirtualSlot? slot = receiver.VirtualSlots.SingleOrDefault(item => item.SlotMethodSymbolId == method.Dispatch.SlotMethodSymbolId);
            if (slot is null) return null;
            method = catalog.Methods.Single(item => item.MethodSymbolId == slot.ImplementationMethodSymbolId);
        }
        else if (dispatchKind != "direct") return null;
        return method.Dispatch.IsAbstract ? null : method;
    }
}
