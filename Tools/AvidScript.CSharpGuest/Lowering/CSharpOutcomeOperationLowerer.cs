using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal static class CSharpOutcomeOperationLowerer
{
    public static bool TryLowerProperty(
        CSharpFunctionLoweringContext context,
        SemanticOperation operation,
        int blockOrdinal,
        List<GuestInstruction> instructions,
        out GuestRegister? result)
    {
        result = null;
        if (operation.Children.Count != 1
            || operation.SymbolId is null
            || !context.TryGetGuestType(operation.Children[0].TypeId, out GuestType outcomeType)
            || outcomeType.Kind != "struct"
            || outcomeType.Fields.Count != 2
            || FindField(outcomeType, CSharpGuestIds.OutcomeStatusField(outcomeType.Id)) is not { } statusField
            || FindField(outcomeType, CSharpGuestIds.OutcomeValueField(outcomeType.Id)) is not { } valueField)
        {
            return false;
        }

        SemanticSymbol? property = context.Document.Symbols.SingleOrDefault(symbol =>
            symbol.Id == operation.SymbolId && symbol.Kind == "property");
        if (property?.Name is not ("Status" or "Value" or "Succeeded" or "Failed" or "Cancelled"))
        {
            return false;
        }

        GuestRegister? receiver = CSharpOperationLowerer.LowerValue(
            context,
            operation.Children[0],
            blockOrdinal,
            instructions);
        if (receiver is null)
        {
            return true;
        }

        if (property.Name is "Status" or "Value")
        {
            GuestField field = property.Name == "Status" ? statusField : valueField;
            result = context.CreateTemporary(field.TypeId, blockOrdinal);
            if (result is not null)
            {
                instructions.Add(new GuestInstruction(
                    "field_load",
                    result.Id,
                    new[] { receiver.Id },
                    field.Id,
                    null,
                    null));
            }
            return true;
        }

        GuestRegister? status = context.CreateTemporary(statusField.TypeId, blockOrdinal);
        GuestRegister? expectedStatus = context.CreateTemporary(statusField.TypeId, blockOrdinal);
        result = context.CreateTemporary(operation.TypeId, blockOrdinal);
        if (status is null || expectedStatus is null || result is null)
        {
            result = null;
            return true;
        }
        instructions.Add(new GuestInstruction(
            "field_load",
            status.Id,
            new[] { receiver.Id },
            statusField.Id,
            null,
            null));
        instructions.Add(new GuestInstruction(
            "constant",
            expectedStatus.Id,
            Array.Empty<string>(),
            null,
            null,
            new GuestConstant(
                "int32",
                property.Name switch
                {
                    "Succeeded" => "1",
                    "Failed" => "2",
                    "Cancelled" => "3",
                    _ => throw new InvalidOperationException(
                        $"Unsupported outcome status property '{property.Name}'."),
                })));
        instructions.Add(new GuestInstruction(
            "binary",
            result.Id,
            new[] { status.Id, expectedStatus.Id },
            null,
            "equals",
            null));
        return true;
    }

    private static GuestField? FindField(GuestType type, string id)
    {
        return type.Fields.SingleOrDefault(field => field.Id == id);
    }
}
