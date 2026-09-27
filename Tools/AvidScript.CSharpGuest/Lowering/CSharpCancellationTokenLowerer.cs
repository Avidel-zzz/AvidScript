using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal static class CSharpCancellationTokenLowerer
{
    internal static GuestRegister? Lower(CSharpFunctionLoweringContext context, SemanticOperation operation,
        int block, List<GuestInstruction> instructions)
    {
        if (CSharpCancellationTokenExecutionContext.Find(context.Document) is null) return null;
        if (operation.Kind == SemanticCancellationTokens.None) {
            var zero = CSharpTaskResultAbi.Constant(context, "type:int64", 0, block, instructions);
            return zero is null ? null : Wrap(zero);
        }
        // Evaluate operands once and left-to-right before reading their identities.
        var left = CSharpOperationLowerer.LowerValue(context, operation.Children[0], block, instructions);
        if (left is null) return null;
        if (operation.Kind == SemanticCancellationTokens.FromAvid) {
            if (!context.TryGetGuestType(left.TypeId, out var type) || type.Fields.Count != 1) return null;
            var identity = Load(left, type.Fields.Single().Id);
            return identity is null ? null : Wrap(identity);
        }
        if (operation.Kind == SemanticCancellationTokens.Compare) {
            // Snapshot the left value before the right expression can mutate its storage.
            var a = Load(left, GuestCancellationTokens.FieldId);
            var right = CSharpOperationLowerer.LowerValue(context, operation.Children[1], block, instructions);
            var b = right is null ? null : Load(right, GuestCancellationTokens.FieldId);
            var result = context.CreateTemporary("type:bool", block);
            if (a is null || b is null || result is null) return null;
            instructions.Add(new("binary", result.Id, new[] { a.Id, b.Id }, null, operation.OperatorKind, null));
            return result;
        }
        if (!CSharpAsyncMemberAssignmentLowerer.CheckTokenReceiver(context, operation, left.Id, instructions)) return null;
        var root = context.CreateTemporary(GuestCancellationTokens.RootTypeId, block);
        var token = context.CreateTemporary("type:int64", block);
        if (root is null || token is null) return null;
        instructions.Add(new("managed_cast", root.Id, new[] { left.Id }, null, null, null));
        instructions.Add(new("call", token.Id, new[] { root.Id }, GuestCancellationTokens.ReadImportId, null, null));
        return Wrap(token);

        GuestRegister? Load(GuestRegister owner, string field) {
            var value = context.CreateTemporary("type:int64", block);
            if (value is not null) instructions.Add(new("field_load", value.Id, new[] { owner.Id }, field, null, null));
            return value;
        }
        GuestRegister? Wrap(GuestRegister identity) {
            var value = context.CreateTemporary(GuestCancellationTokens.TypeId, block);
            if (value is null) return null;
            instructions.Add(new("stack_alloc", value.Id, System.Array.Empty<string>(), null, null, null));
            instructions.Add(new("field_store", null, new[] { value.Id, identity.Id }, GuestCancellationTokens.FieldId, null, null));
            return value;
        }
    }
}
