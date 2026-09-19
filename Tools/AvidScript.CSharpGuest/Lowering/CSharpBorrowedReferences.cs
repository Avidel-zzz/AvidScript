using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal static class CSharpBorrowedReferences
{
    public static bool Enabled(SemanticDocument document) => document.ClosureEnvironments.Count != 0;
    public static string Type(string pointee) => "type:$borrow:" + pointee;
    public static string Parameter(SemanticDocument document, string type, string refKind) => refKind == "none" ? type
        : Enabled(document) ? Type(type) : CSharpGuestIds.AddressTypeId;
    public static bool IsBorrowed(CSharpFunctionLoweringContext context, GuestRegister value) =>
        context.TryGetGuestType(value.TypeId, out GuestType type) && type.Kind == GuestBorrowedReference.Kind;
    public static bool BorrowedReceiver(CSharpFunctionLoweringContext context, SemanticCallable callable) =>
        Enabled(context.Document) && callable.Import is null && !callable.IsStatic
        && context.TryGetGuestType(callable.ContainingTypeId, out GuestType type) && type.Kind == "struct";

    public static void AddTypes(SemanticDocument document, List<GuestType> types)
    {
        if (!Enabled(document)) return;
        foreach (GuestType type in types.Where(type => type.Kind != "void" && type.Kind != "managed_ref").ToArray())
            types.Add(GuestBorrowedReference.Declare(Type(type.Id), type.Id, CSharpClosureLayout.ObjectType, CSharpGuestIds.AddressTypeId));
    }

    public static GuestRegister? Address(CSharpFunctionLoweringContext context, SemanticOperation operation, int block,
        List<GuestInstruction> instructions)
    {
        if (operation.Kind is "argument" or "declaration_expression" && operation.Children.Count == 1)
            return Address(context, operation.Children[0], block, instructions);
        if (operation.Kind == "flow_capture_reference" && context.TryGetCaptureTarget(operation.CaptureId, out SemanticOperation target))
            return Address(context, target, block, instructions);
        if (context.ClosureCells.TryAddress(operation.SymbolId, operation.TypeId, block, instructions, out GuestRegister? cell)) return cell;
        if (operation.Kind == "instance_reference" && context.ThisRegister is { } instance && IsBorrowed(context, instance)) return instance;
        if (operation.Kind is "local_reference" or "parameter_reference" && context.TryGetStorage(operation.SymbolId, out GuestRegister storage))
        {
            if (IsBorrowed(context, storage)) return storage;
            if (operation.Kind == "parameter_reference" && context.TryGetParameter(operation.SymbolId, out SemanticCallableParameter parameter)
                && parameter.RefKind != "none")
            { context.Add("ASCG1024", "A raw Host reference cannot become a traced Guest borrow without an explicit boundary adapter."); return null; }
            return Storage(context, storage, block, instructions);
        }
        if (operation.Kind == "field_reference" && operation.Children.Count == 1 && !context.IsUeProperty(operation.SymbolId))
        {
            GuestRegister? owner = Address(context, operation.Children[0], block, instructions);
            GuestRegister? reference = context.CreateTemporary(Type(operation.TypeId!), block);
            if (owner is null || reference is null) return null;
            instructions.Add(new("borrow_field", reference.Id, new[] { owner.Id }, operation.SymbolId, null, null));
            return reference;
        }
        context.Add("ASCG1024", $"Operation '{operation.Kind}' has no stable synchronous borrowed storage.");
        return null;
    }

    public static GuestRegister? Storage(CSharpFunctionLoweringContext context, GuestRegister storage, int block, List<GuestInstruction> instructions)
    {
        GuestRegister? reference = context.CreateTemporary(Type(storage.TypeId), block);
        if (reference is not null) instructions.Add(new("borrow_address", reference.Id, Array.Empty<string>(), storage.Id, null, null));
        return reference;
    }

    public static GuestRegister? Receiver(CSharpFunctionLoweringContext context, SemanticOperation operation, int block,
        List<GuestInstruction> instructions, SemanticCallable callable)
    {
        bool defensiveCopy = ReadonlyStorage(context, operation)
            && !context.Document.Symbols.Any(symbol => symbol.Id == callable.MethodSymbolId && symbol.IsReadonly);
        if (!defensiveCopy && (operation.Kind is "local_reference" or "parameter_reference" or "instance_reference" or "flow_capture_reference"
            || (operation.Kind == "field_reference" && !context.IsUeProperty(operation.SymbolId))))
            return Address(context, operation, block, instructions);
        GuestRegister? temporary = CSharpOperationLowerer.LowerValue(context, operation, block, instructions);
        return temporary is null ? null : Storage(context, temporary, block, instructions);
    }

    private static bool ReadonlyStorage(CSharpFunctionLoweringContext context, SemanticOperation operation)
    {
        if (operation.Kind == "flow_capture_reference" && context.TryGetCaptureTarget(operation.CaptureId, out SemanticOperation target))
            return ReadonlyStorage(context, target);
        if (operation.Kind == "parameter_reference" && context.TryGetParameter(operation.SymbolId, out SemanticCallableParameter parameter))
            return parameter.RefKind == "in";
        if (operation.Kind == "instance_reference")
            return context.Document.Symbols.Any(symbol => symbol.Id == context.Callable.MethodSymbolId && symbol.IsReadonly);
        if (operation.Kind != "field_reference" || operation.Children.Count != 1) return false;
        bool writableConstruction = context.Callable.IsConstructor && operation.Children[0].Kind == "instance_reference";
        return (!writableConstruction && context.Document.Symbols.Any(symbol => symbol.Id == operation.SymbolId && symbol.IsReadonly))
            || ReadonlyStorage(context, operation.Children[0]);
    }

    public static GuestRegister? Read(CSharpFunctionLoweringContext context, GuestRegister reference, string type, int block, List<GuestInstruction> instructions)
    {
        GuestRegister? value = context.CreateTemporary(type, block);
        if (value is not null) instructions.Add(new("borrow_load", value.Id, new[] { reference.Id }, type, null, null));
        return value;
    }
}
