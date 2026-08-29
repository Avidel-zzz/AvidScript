using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal static class CSharpAggregateOperationLowerer
{
    public static GuestRegister? Allocate(
        CSharpFunctionLoweringContext context,
        string? typeId,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        if (!context.TryGetGuestType(typeId, out GuestType type) || type.Kind != "struct")
        {
            context.Add("ASCG1004", $"Block {blockOrdinal} cannot allocate non-struct type '{typeId}'.");
            return null;
        }

        GuestRegister? result = context.CreateTemporary(typeId, blockOrdinal);
        if (result is null)
        {
            return null;
        }

        instructions.Add(new GuestInstruction(
            "stack_alloc", result.Id, Array.Empty<string>(), null, null, null));
        return result;
    }

    public static GuestRegister? LowerFieldLoad(
        CSharpFunctionLoweringContext context,
        SemanticOperation operation,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        if (operation.Children.Count != 1 || operation.SymbolId is null)
        {
            return Malformed(context, operation, blockOrdinal);
        }

        if (context.TryGetGuestType(operation.Children[0].TypeId, out GuestType receiverType)
            && receiverType.Kind is "factory_ref" or "object_type_ref")
        {
            if (!CSharpObjectCapabilityPolicy.IsOrdinalField(
                context.Document,
                operation.SymbolId,
                operation.Children[0].TypeId))
            {
                context.Add("ASCG1004", $"Block {blockOrdinal} object capability ordinal field is malformed.");
                return null;
            }

            GuestRegister? capability = CSharpOperationLowerer.LowerValue(
                context,
                operation.Children[0],
                blockOrdinal,
                instructions);
            GuestRegister? ordinal = context.CreateTemporary(operation.TypeId, blockOrdinal);
            if (capability is null || ordinal is null)
            {
                return null;
            }

            instructions.Add(new GuestInstruction(
                "convert",
                ordinal.Id,
                new[] { capability.Id },
                null,
                receiverType.Kind + "_ordinal",
                null));
            return ordinal;
        }

        if (context.TryGetGuestType(operation.Children[0].TypeId, out receiverType)
            && receiverType.Kind == "class_ref")
        {
            if (!CSharpClassReferencePolicy.IsOrdinalField(
                context.Document,
                operation.SymbolId,
                operation.Children[0].TypeId))
            {
                context.Add("ASCG1004", $"Block {blockOrdinal} class reference ordinal field is malformed.");
                return null;
            }

            GuestRegister? classReference = CSharpOperationLowerer.LowerValue(
                context,
                operation.Children[0],
                blockOrdinal,
                instructions);
            GuestRegister? ordinal = context.CreateTemporary(operation.TypeId, blockOrdinal);
            if (classReference is null || ordinal is null)
            {
                return null;
            }

            instructions.Add(new GuestInstruction(
                "convert",
                ordinal.Id,
                new[] { classReference.Id },
                null,
                "class_ref_ordinal",
                null));
            return ordinal;
        }

        GuestRegister? aggregate = CSharpOperationLowerer.LowerValue(
            context,
            operation.Children[0],
            blockOrdinal,
            instructions);
        GuestRegister? result = context.CreateTemporary(operation.TypeId, blockOrdinal);
        if (aggregate is null || result is null)
        {
            return null;
        }

        instructions.Add(new GuestInstruction(
            "field_load",
            result.Id,
            new[] { aggregate.Id },
            operation.SymbolId,
            null,
            null));
        return result;
    }

    public static bool StoreField(
        CSharpFunctionLoweringContext context,
        SemanticOperation target,
        GuestRegister value,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        if (target.Children.Count != 1 || target.SymbolId is null)
        {
            Malformed(context, target, blockOrdinal);
            return false;
        }

        GuestRegister? aggregate = CSharpOperationLowerer.LowerValue(
            context,
            target.Children[0],
            blockOrdinal,
            instructions);
        if (aggregate is null)
        {
            return false;
        }

        instructions.Add(new GuestInstruction(
            "field_store",
            null,
            new[] { aggregate.Id, value.Id },
            target.SymbolId,
            null,
            null));
        return true;
    }

    public static GuestRegister? LowerInstance(
        CSharpFunctionLoweringContext context,
        SemanticOperation operation,
        int blockOrdinal)
    {
        if (operation.Children.Count != 0 || context.ThisRegister is null)
        {
            return Malformed(context, operation, blockOrdinal);
        }

        return context.ThisRegister;
    }

    public static GuestRegister? LowerFieldPath(
        CSharpFunctionLoweringContext context,
        GuestRegister aggregate,
        IReadOnlyList<string> fieldPath,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        GuestRegister current = aggregate;
        foreach (string fieldId in fieldPath)
        {
            if (!context.TryGetGuestType(current.TypeId, out GuestType aggregateType)
                || aggregateType.Kind != "struct"
                || aggregateType.Fields.SingleOrDefault(field => field.Id == fieldId)
                    is not { } field)
            {
                context.Add("ASCG1004", $"Block {blockOrdinal} aggregate field path is not present in '{current.TypeId}'.");
                return null;
            }
            GuestRegister? value = context.CreateTemporary(field.TypeId, blockOrdinal);
            if (value is null)
            {
                return null;
            }
            instructions.Add(new GuestInstruction(
                "field_load",
                value.Id,
                new[] { current.Id },
                field.Id,
                null,
                null));
            current = value;
        }
        return current;
    }

    public static GuestRegister? LowerStorageAddress(
        CSharpFunctionLoweringContext context,
        GuestRegister storage,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        GuestRegister? address = context.CreateTemporary(
            CSharpGuestIds.AddressTypeId,
            blockOrdinal);
        if (address is null)
        {
            return null;
        }
        instructions.Add(new GuestInstruction(
            "address_of",
            address.Id,
            Array.Empty<string>(),
            storage.Id,
            null,
            null));
        return address;
    }

    private static GuestRegister? Malformed(
        CSharpFunctionLoweringContext context,
        SemanticOperation operation,
        int blockOrdinal)
    {
        context.Add("ASCG1004", $"Block {blockOrdinal} aggregate operation '{operation.Kind}' is malformed.");
        return null;
    }
}
