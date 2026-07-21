using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal sealed record CSharpGameplayEventLoweringResult(
    GuestFunction Function,
    GuestExport Export);

internal static class CSharpGameplayEventLowerer
{
    public static CSharpGameplayEventLoweringResult? Lower(
        SemanticDocument document,
        IReadOnlyDictionary<string, GuestType> guestTypes,
        IReadOnlyList<GuestFunction> loweredFunctions,
        List<GuestDiagnostic> diagnostics)
    {
        if (document.GameplayEventCallbacks.Count == 0)
        {
            return null;
        }

        if (document.Callables.Any(callable =>
                string.Equals(
                    callable.Export?.Name,
                    CSharpGuestIds.GameplayEventExportName,
                    StringComparison.Ordinal)))
        {
            Add(diagnostics, "The compiler-generated gameplay router conflicts with an explicit avid_on_gameplay_event export.");
            return null;
        }

        if (!guestTypes.TryGetValue("type:int32", out GuestType? int32Type)
            || !guestTypes.TryGetValue("type:float32", out GuestType? float32Type)
            || !guestTypes.TryGetValue("type:void", out GuestType? voidType))
        {
            Add(diagnostics, "The semantic artifact does not contain the primitive gameplay router ABI types.");
            return null;
        }

        Dictionary<string, SemanticCallable> callablesById = document.Callables.ToDictionary(
            callable => callable.MethodSymbolId,
            StringComparer.Ordinal);
        HashSet<string> loweredFunctionIds = loweredFunctions
            .Select(function => function.Id)
            .ToHashSet(StringComparer.Ordinal);
        SemanticGameplayEventCallback[] callbacks = document.GameplayEventCallbacks
            .OrderBy(callback => callback.EventType)
            .ToArray();
        foreach (SemanticGameplayEventCallback callback in callbacks)
        {
            if (!callablesById.TryGetValue(callback.MethodSymbolId, out SemanticCallable? callable)
                || !loweredFunctionIds.Contains(CSharpGuestIds.Function(callback.MethodSymbolId))
                || !ValidateCallbackPayload(callback, callable, guestTypes, int32Type.Id, float32Type.Id, diagnostics))
            {
                Add(diagnostics, $"Gameplay callback '{callback.Name}' has no compatible lowered function or payload layout.");
                return null;
            }
        }

        GuestRegister eventType = Parameter("event_type", int32Type.Id);
        GuestRegister primaryId = Parameter("primary_id", int32Type.Id);
        GuestRegister secondaryId = Parameter("secondary_id", int32Type.Id);
        GuestRegister objectSlot = Parameter("object_slot", int32Type.Id);
        GuestRegister objectGeneration = Parameter("object_generation", int32Type.Id);
        GuestRegister x = Parameter("x", float32Type.Id);
        GuestRegister y = Parameter("y", float32Type.Id);
        GuestRegister z = Parameter("z", float32Type.Id);
        GuestRegister[] parameters =
        {
            eventType,
            primaryId,
            secondaryId,
            objectSlot,
            objectGeneration,
            x,
            y,
            z,
        };
        List<GuestRegister> locals = new();
        List<GuestBasicBlock> blocks = new();
        for (int index = 0; index < callbacks.Length; ++index)
        {
            SemanticGameplayEventCallback callback = callbacks[index];
            string checkBlockId = CSharpGuestIds.GameplayEventCheckBlock(callback.EventType);
            string callBlockId = CSharpGuestIds.GameplayEventCallBlock(callback.EventType);
            string falseBlockId = index + 1 < callbacks.Length
                ? CSharpGuestIds.GameplayEventCheckBlock(callbacks[index + 1].EventType)
                : CSharpGuestIds.GameplayEventReturnBlockId;
            GuestRegister eventConstant = Local(callback.EventType, "event_type_constant", int32Type.Id, locals);
            GuestRegister condition = Local(callback.EventType, "event_type_matches", int32Type.Id, locals);
            GuestInstruction[] checkInstructions =
            {
                new(
                    "constant",
                    eventConstant.Id,
                    Array.Empty<string>(),
                    null,
                    null,
                    new GuestConstant("int32", callback.EventType.ToString(CultureInfo.InvariantCulture))),
                new(
                    "binary",
                    condition.Id,
                    new[] { eventType.Id, eventConstant.Id },
                    null,
                    "equals",
                    null),
            };
            blocks.Add(new GuestBasicBlock(
                checkBlockId,
                checkInstructions,
                new GuestTerminator("branch_if", condition.Id, callBlockId, falseBlockId, null)));

            SemanticCallable callable = callablesById[callback.MethodSymbolId];
            List<GuestInstruction> callInstructions = new();
            string[] payloadIds = BuildPayload(
                callback,
                callable,
                guestTypes,
                primaryId,
                secondaryId,
                objectSlot,
                objectGeneration,
                x,
                y,
                z,
                locals,
                callInstructions);
            callInstructions.Add(new GuestInstruction(
                "call",
                null,
                payloadIds,
                CSharpGuestIds.Function(callback.MethodSymbolId),
                null,
                null));
            blocks.Add(new GuestBasicBlock(
                callBlockId,
                callInstructions,
                Return()));
        }

        blocks.Add(new GuestBasicBlock(
            CSharpGuestIds.GameplayEventReturnBlockId,
            Array.Empty<GuestInstruction>(),
            Return()));
        GuestFunction function = new(
            CSharpGuestIds.GameplayEventFunctionId,
            parameters,
            locals,
            voidType.Id,
            CSharpGuestIds.GameplayEventCheckBlock(callbacks[0].EventType),
            blocks);
        return new CSharpGameplayEventLoweringResult(
            function,
            new GuestExport(CSharpGuestIds.GameplayEventExportName, function.Id));
    }

    private static bool ValidateCallbackPayload(
        SemanticGameplayEventCallback callback,
        SemanticCallable callable,
        IReadOnlyDictionary<string, GuestType> guestTypes,
        string int32TypeId,
        string float32TypeId,
        List<GuestDiagnostic> diagnostics)
    {
        if (callback.EventType is >= 1 and <= 3)
        {
            return callable.Parameters.Count == 2
                && TryGetStruct(guestTypes, callable.Parameters[0].TypeId, out GuestType actor)
                && actor.Fields.Count == 2
                && HasField(actor, "Slot", int32TypeId)
                && HasField(actor, "Generation", int32TypeId)
                && TryGetStruct(guestTypes, callable.Parameters[1].TypeId, out GuestType vector)
                && HasVectorFields(vector, float32TypeId);
        }

        if (callback.EventType == 4
            && callable.Parameters.Count == 1
            && TryGetStruct(guestTypes, callable.Parameters[0].TypeId, out GuestType input)
            && input.Fields.Count == 3
            && HasField(input, "ActionId", int32TypeId)
            && HasField(input, "TriggerEvent", int32TypeId)
            && FindField(input, "Value") is { } valueField
            && TryGetStruct(guestTypes, valueField.TypeId, out GuestType inputVector)
            && HasVectorFields(inputVector, float32TypeId))
        {
            return true;
        }

        Add(diagnostics, $"Gameplay callback '{callback.Name}' uses an incompatible generated value type layout.");
        return false;
    }

    private static string[] BuildPayload(
        SemanticGameplayEventCallback callback,
        SemanticCallable callable,
        IReadOnlyDictionary<string, GuestType> guestTypes,
        GuestRegister primaryId,
        GuestRegister secondaryId,
        GuestRegister objectSlot,
        GuestRegister objectGeneration,
        GuestRegister x,
        GuestRegister y,
        GuestRegister z,
        List<GuestRegister> locals,
        List<GuestInstruction> instructions)
    {
        if (callback.EventType is >= 1 and <= 3)
        {
            GuestType actorType = guestTypes[callable.Parameters[0].TypeId];
            GuestType vectorType = guestTypes[callable.Parameters[1].TypeId];
            GuestRegister actor = Allocate(callback.EventType, "actor", actorType.Id, locals, instructions);
            Store(actor, actorType, "Slot", objectSlot, instructions);
            Store(actor, actorType, "Generation", objectGeneration, instructions);
            GuestRegister vector = BuildVector(
                callback.EventType,
                "vector",
                vectorType,
                x,
                y,
                z,
                locals,
                instructions);
            return new[] { actor.Id, vector.Id };
        }

        GuestType inputType = guestTypes[callable.Parameters[0].TypeId];
        GuestField valueField = FindField(inputType, "Value")!;
        GuestType inputVectorType = guestTypes[valueField.TypeId];
        GuestRegister inputVector = BuildVector(
            callback.EventType,
            "input_vector",
            inputVectorType,
            x,
            y,
            z,
            locals,
            instructions);
        GuestRegister input = Allocate(callback.EventType, "input", inputType.Id, locals, instructions);
        Store(input, inputType, "ActionId", primaryId, instructions);
        Store(input, inputType, "TriggerEvent", secondaryId, instructions);
        Store(input, inputType, "Value", inputVector, instructions);
        return new[] { input.Id };
    }

    private static GuestRegister BuildVector(
        int eventType,
        string name,
        GuestType type,
        GuestRegister x,
        GuestRegister y,
        GuestRegister z,
        List<GuestRegister> locals,
        List<GuestInstruction> instructions)
    {
        GuestRegister vector = Allocate(eventType, name, type.Id, locals, instructions);
        Store(vector, type, "X", x, instructions);
        Store(vector, type, "Y", y, instructions);
        Store(vector, type, "Z", z, instructions);
        return vector;
    }

    private static GuestRegister Allocate(
        int eventType,
        string name,
        string typeId,
        List<GuestRegister> locals,
        List<GuestInstruction> instructions)
    {
        GuestRegister value = Local(eventType, name, typeId, locals);
        instructions.Add(new GuestInstruction(
            "stack_alloc",
            value.Id,
            Array.Empty<string>(),
            null,
            null,
            null));
        return value;
    }

    private static void Store(
        GuestRegister aggregate,
        GuestType aggregateType,
        string fieldName,
        GuestRegister value,
        List<GuestInstruction> instructions)
    {
        instructions.Add(new GuestInstruction(
            "field_store",
            null,
            new[] { aggregate.Id, value.Id },
            FindField(aggregateType, fieldName)!.Id,
            null,
            null));
    }

    private static GuestRegister Parameter(string name, string typeId)
    {
        return new GuestRegister(CSharpGuestIds.GameplayEventParameter(name), typeId);
    }

    private static GuestRegister Local(
        int eventType,
        string name,
        string typeId,
        List<GuestRegister> locals)
    {
        GuestRegister value = new(CSharpGuestIds.GameplayEventLocal(eventType, name), typeId);
        locals.Add(value);
        return value;
    }

    private static GuestTerminator Return()
    {
        return new GuestTerminator("return", null, null, null, null);
    }

    private static bool TryGetStruct(
        IReadOnlyDictionary<string, GuestType> guestTypes,
        string typeId,
        out GuestType type)
    {
        return guestTypes.TryGetValue(typeId, out type!)
            && string.Equals(type.Kind, "struct", StringComparison.Ordinal);
    }

    private static bool HasVectorFields(GuestType type, string float32TypeId)
    {
        return type.Fields.Count == 3
            && HasField(type, "X", float32TypeId)
            && HasField(type, "Y", float32TypeId)
            && HasField(type, "Z", float32TypeId);
    }

    private static bool HasField(GuestType type, string name, string typeId)
    {
        return FindField(type, name) is { } field
            && string.Equals(field.TypeId, typeId, StringComparison.Ordinal);
    }

    private static GuestField? FindField(GuestType type, string name)
    {
        GuestField? result = null;
        foreach (GuestField field in type.Fields.Where(field =>
            string.Equals(field.Name, name, StringComparison.Ordinal)))
        {
            if (result is not null)
            {
                return null;
            }

            result = field;
        }

        return result;
    }

    private static void Add(List<GuestDiagnostic> diagnostics, string message)
    {
        diagnostics.Add(new GuestDiagnostic("ASCG1007", "error", message, null));
    }
}
