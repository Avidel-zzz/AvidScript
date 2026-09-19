using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.GuestIr;

internal static class GuestBorrowedReferenceValidator
{
    public static void ValidateContracts(GuestValidationContext context)
    {
        GuestModule module = context.Module;
        if (!module.Types.Any(type => type.Kind == GuestBorrowedReference.Kind)) return;
        bool Has(string id) => GuestBorrowedReference.Contains(context.Types, id);
        if (module.SchemaVersion < 6) Add(context, "Borrowed references require IR 6/1.5.");
        if (module.Types.Any(type => type.Kind != GuestBorrowedReference.Kind && Has(type.Id))
            || module.Types.Any(type => type.Kind == GuestBorrowedReference.Kind && type.ElementTypeId is { } pointee && Has(pointee)))
            Add(context, "Borrowed references cannot be embedded in aggregates or reference other borrowed descriptors.");
        if (module.Globals.Any(global => Has(global.TypeId)) || module.DataSegments.Any(segment => Has(segment.TypeId))
            || module.Functions.Any(function => Has(function.ReturnTypeId))
            || module.FunctionReferences.Any(contract => Has(contract.ReturnTypeId)))
            Add(context, "Borrowed references cannot escape through persistent storage or return values.");
    }

    public static void ValidateInstruction(GuestValidationContext context, GuestFunction function, GuestInstruction instruction,
        GuestRegister? result, IReadOnlyList<GuestRegister?> operands, IReadOnlyDictionary<string, GuestRegister> values)
    {
        GuestType? Type(GuestRegister? register) => register is not null && context.Types.TryGetValue(register.TypeId, out GuestType? type) ? type : null;
        GuestType? target = Type(result), first = operands.Count > 0 ? Type(operands[0]) : null;
        bool borrowed = target?.Kind == GuestBorrowedReference.Kind || operands.Any(operand => Type(operand)?.Kind == GuestBorrowedReference.Kind);
        if (!instruction.Op.StartsWith("borrow_", StringComparison.Ordinal))
        {
            if (borrowed && instruction.Op is not ("copy" or "local_load" or "local_store" or "call" or "call_indirect" or "memory_copy"))
                Add(context, $"Function '{function.Id}' cannot expose or forge a borrowed descriptor through '{instruction.Op}'.");
            if (instruction.Op == "address_of" && instruction.TargetId is { } addressed && values.TryGetValue(addressed, out GuestRegister? value)
                && Type(value)?.Kind == GuestBorrowedReference.Kind)
                Add(context, "The raw address of a borrowed descriptor cannot escape.");
            return;
        }
        bool valid = context.Module.SchemaVersion >= 6 && instruction.OperatorKind is null && instruction.Constant is null;
        if (instruction.Op == "borrow_address")
            valid &= target?.Kind == GuestBorrowedReference.Kind && operands.Count == 0
                && instruction.TargetId is { } id && values.TryGetValue(id, out GuestRegister? storage) && target.ElementTypeId == storage.TypeId;
        else if (instruction.Op is "borrow_managed" or "borrow_field")
        {
            bool owner = instruction.Op == "borrow_managed" ? first?.Kind == "managed_ref" : first?.Kind == GuestBorrowedReference.Kind;
            GuestField? field = owner && first?.ElementTypeId is { } payload && context.Types.TryGetValue(payload, out GuestType? aggregate)
                && aggregate.Kind == "struct" ? aggregate.Fields.FirstOrDefault(item => item.Id == instruction.TargetId) : null;
            valid &= target?.Kind == GuestBorrowedReference.Kind && operands.Count == 1 && field is not null && target.ElementTypeId == field.TypeId;
        }
        else if (instruction.Op is "borrow_load" or "borrow_store")
            valid &= first?.Kind == GuestBorrowedReference.Kind && instruction.TargetId == first.ElementTypeId
                && (instruction.Op == "borrow_load" ? operands.Count == 1 && result?.TypeId == first.ElementTypeId
                    : operands.Count == 2 && result is null && operands[1]?.TypeId == first.ElementTypeId);
        else valid = false;
        if (!valid) Add(context, $"Function '{function.Id}' has invalid '{instruction.Op}' operands or pointee type.");
    }

    private static void Add(GuestValidationContext context, string message) => context.Add("ASIR1014", message);
}
