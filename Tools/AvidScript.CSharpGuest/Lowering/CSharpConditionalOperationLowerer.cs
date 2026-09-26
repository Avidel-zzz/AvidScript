using System;
using System.Collections.Generic;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal static class CSharpConditionalOperationLowerer
{
    internal static GuestRegister? Lower(CSharpFunctionLoweringContext context,
        SemanticOperation operation, int blockOrdinal, List<GuestInstruction> instructions)
    {
        if (operation.Children.Count != 3 || operation.Children[0].TypeId != "type:bool"
            || operation.TypeId is null || operation.TypeId == "type:void"
            || operation.Children[1].TypeId != operation.TypeId || operation.Children[2].TypeId != operation.TypeId
            || operation.SymbolId is not null || operation.OperatorKind is not null || operation.IsLifted)
        {
            context.Add("ASCG1004", $"Block {blockOrdinal} conditional needs a Boolean condition and two values of its result type.");
            return null;
        }
        GuestRegister? condition = CSharpOperationLowerer.LowerValue(context, operation.Children[0], blockOrdinal, instructions);
        int trueStart = instructions.Count;
        GuestRegister? whenTrue = CSharpOperationLowerer.LowerValue(context, operation.Children[1], blockOrdinal, instructions);
        int falseStart = instructions.Count;
        GuestRegister? whenFalse = CSharpOperationLowerer.LowerValue(context, operation.Children[2], blockOrdinal, instructions);
        GuestRegister? result = context.CreateTemporary(operation.TypeId, blockOrdinal);
        GuestRegister? merge = context.CreateTemporary(operation.TypeId, blockOrdinal);
        if (condition is null || whenTrue is null || whenFalse is null || result is null || merge is null) return null;
        context.ShortCircuitFlow.RecordConditional(result, condition, whenTrue, whenFalse, merge,
            falseStart - trueStart, instructions.Count - falseStart);
        // Replaced before validation by branches and scalar stores or aggregate
        // copies. Neither arm is executed eagerly in the published Guest CFG.
        instructions.Add(new GuestInstruction("local_load", result.Id, Array.Empty<string>(), merge.Id, null, null));
        return result;
    }
}
