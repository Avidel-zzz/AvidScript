using System;
using System.Collections.Generic;
using System.Globalization;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal static class CSharpClassReferenceLowerer
{
    public static bool TryLowerObjectCreation(
        CSharpFunctionLoweringContext context,
        SemanticOperation operation,
        int blockOrdinal,
        List<GuestInstruction> instructions,
        out GuestRegister? result)
    {
        result = null;
        if (!context.TryGetGuestType(operation.TypeId, out GuestType type)
            || type.Kind != "class_ref")
        {
            return false;
        }

        if (!context.TryGetCallTarget(operation.SymbolId, out SemanticCallable constructor, out _)
            || !CSharpClassReferencePolicy.IsIntrinsicConstructor(
                context.Document,
                operation.TypeId,
                constructor)
            || operation.Children.Count != 1)
        {
            context.Add("ASCG1004", $"Block {blockOrdinal} class reference construction is malformed.");
            return true;
        }

        SemanticOperation argument = operation.Children[0].Kind == "argument"
            && operation.Children[0].Children.Count == 1
                ? operation.Children[0].Children[0]
                : operation.Children[0];
        if (argument.Kind != "literal"
            || argument.Constant is null
            || !string.Equals(argument.Constant.Kind, "int32", StringComparison.Ordinal)
            || !int.TryParse(
                argument.Constant.Value,
                NumberStyles.Integer,
                CultureInfo.InvariantCulture,
                out int ordinal)
            || ordinal < 0
            || operation.TypeId is null
            || !CSharpClassReferencePolicy.IsAuthorizedOrdinal(
                context.Document,
                operation.TypeId,
                ordinal))
        {
            context.Add(
                "ASCG1004",
                $"Block {blockOrdinal} class reference requires a generated ordinal published for its nominal wrapper.");
            return true;
        }

        result = context.CreateTemporary(operation.TypeId, blockOrdinal);
        if (result is not null)
        {
            instructions.Add(new GuestInstruction(
                "constant",
                result.Id,
                Array.Empty<string>(),
                null,
                null,
                new GuestConstant("class_ref", ordinal.ToString(CultureInfo.InvariantCulture))));
        }
        return true;
    }
}
