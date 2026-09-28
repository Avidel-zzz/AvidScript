using System.Collections.Generic;

namespace AvidScript.GuestIr;

internal static class GuestStaticStorageValidator
{
    public static void ValidateContracts(GuestValidationContext context)
    {
        GuestModule artifact = context.Artifact;
        bool composable = GuestComposableCapabilities.Has(context.InputArtifact, GuestComposableCapabilities.StaticStorage);
        if (!GuestStaticStorage.IsVersion(artifact) && !composable)
        {
            if (artifact.StaticStorage is not null)
                Add(context, GuestComposableCapabilities.IsVersion(context.InputArtifact)
                    ? "IR 35 static storage requires its declared capability."
                    : "Static storage requires Guest IR 27/1.26.");
            return;
        }
        if (artifact.StaticStorage is not { Slots: { Count: > 0 and <= GuestManagedHeap.MaxStaticSlots } } storage)
        {
            Add(context, "Static storage requires a bounded nonempty slot table and an existing execution profile.");
            return;
        }
        bool supportedBase = composable
            ? storage.BaseSchemaVersion == 14 && storage.BaseIrVersion == "1.13"
            : GuestStaticAsyncExecution.IsVersion(artifact)
                ? storage.BaseSchemaVersion == GuestAsyncSynchronousExceptions.SchemaVersion
                    && storage.BaseIrVersion == GuestAsyncSynchronousExceptions.IrVersion
                : storage.BaseSchemaVersion is >= 4 and <= 26;
        if (!supportedBase)
        {
            Add(context, "Static storage requires a bounded nonempty slot table and an existing execution profile.");
            return;
        }
        foreach (GuestStaticSlot slot in storage.Slots)
            if (!context.Types.TryGetValue(slot.TypeId, out GuestType? type) || type.Kind != "managed_ref")
                Add(context, $"Static slot '{slot.Id}' requires a declared managed reference type.");
    }

    public static void ValidateInstruction(GuestValidationContext context, GuestFunction function,
        GuestInstruction instruction, GuestRegister? result, IReadOnlyList<GuestRegister?> operands)
    {
        bool valid = (GuestStaticStorage.IsVersion(context.Artifact)
            || GuestComposableCapabilities.Has(context.InputArtifact, GuestComposableCapabilities.StaticStorage))
            && context.Artifact.StaticStorage is not null
            && instruction.Constant is null && instruction.OperatorKind is null && instruction.TargetId is not null
            && context.StaticSlots.TryGetValue(instruction.TargetId, out GuestStaticSlot? slot)
            && (instruction.Op == GuestStaticStorage.GetOp
                ? result?.TypeId == slot.TypeId && operands.Count == 0
                : result is null && operands.Count == 1 && operands[0]?.TypeId == slot.TypeId);
        if (!valid) Add(context, $"Function '{function.Id}' has invalid static slot access '{instruction.Op}'.");
    }

    private static void Add(GuestValidationContext context, string message) => context.Add("ASIR1035", message);
}
