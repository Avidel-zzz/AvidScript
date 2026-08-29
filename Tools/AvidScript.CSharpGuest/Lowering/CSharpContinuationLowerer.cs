using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal sealed record CSharpContinuationLoweringResult(
    GuestFunction Function,
    GuestExport Export);

internal static class CSharpContinuationLowerer
{
    private sealed record DispatchTarget(
        int CallbackId,
        string PayloadKind,
        string FunctionId);

    public static CSharpContinuationLoweringResult? Lower(
        SemanticDocument document,
        IReadOnlyDictionary<string, GuestType> guestTypes,
        IReadOnlyList<GuestFunction> loweredFunctions,
        IReadOnlyList<CSharpAsyncResumeRoute> asyncRoutes,
        List<GuestDiagnostic> diagnostics)
    {
        if (document.ContinuationCallbacks.Count == 0 && asyncRoutes.Count == 0)
        {
            return null;
        }

        bool version2 = document.SchemaVersion >= 11;
        string exportName = version2
            ? CSharpGuestIds.ContinuationV2ExportName
            : CSharpGuestIds.ContinuationExportName;
        string functionId = version2
            ? CSharpGuestIds.ContinuationV2FunctionId
            : CSharpGuestIds.ContinuationFunctionId;
        if (document.Callables.Any(callable => string.Equals(
            callable.Export?.Name,
            exportName,
            StringComparison.Ordinal)))
        {
            Add(diagnostics, $"The compiler-generated continuation router conflicts with an explicit {exportName} export.");
            return null;
        }

        if (!guestTypes.TryGetValue(CSharpGuestIds.Int32TypeId, out GuestType? int32Type)
            || !guestTypes.TryGetValue(CSharpGuestIds.Int64TypeId, out GuestType? int64Type)
            || !guestTypes.TryGetValue("type:void", out GuestType? voidType))
        {
            Add(diagnostics, "The semantic artifact does not contain the primitive continuation ABI types.");
            return null;
        }

        Dictionary<string, SemanticCallable> callablesById = document.Callables.ToDictionary(
            callable => callable.MethodSymbolId,
            StringComparer.Ordinal);
        HashSet<string> loweredFunctionIds = loweredFunctions
            .Select(function => function.Id)
            .ToHashSet(StringComparer.Ordinal);
        SemanticContinuationCallback[] callbacks = document.ContinuationCallbacks
            .OrderBy(callback => callback.CallbackId)
            .ToArray();
        bool hasObjectPayload = callbacks.Any(callback =>
                callback.PayloadKind == SemanticContinuationCallback.ObjectPayloadKind)
            || asyncRoutes.Any(route =>
                route.PayloadKind == SemanticContinuationCallback.ObjectPayloadKind);
        GuestType? continuationStatusType = null;
        GuestType? loadedObjectType = null;
        if (hasObjectPayload
            && (!version2
                || !TryGetObjectPayloadTypes(
                    guestTypes,
                    int32Type.Id,
                    out continuationStatusType,
                    out loadedObjectType)))
        {
            Add(diagnostics, "The semantic artifact does not contain the required continuation object-payload layouts.");
            return null;
        }

        foreach (SemanticContinuationCallback callback in callbacks)
        {
            if (!callablesById.TryGetValue(callback.MethodSymbolId, out SemanticCallable? callable)
                || !HasCompatibleParameters(callback, callable, version2)
                || !loweredFunctionIds.Contains(CSharpGuestIds.Function(callback.MethodSymbolId)))
            {
                Add(diagnostics, $"Continuation callback '{callback.CallbackId}' has no compatible lowered function.");
                return null;
            }
        }

        foreach (CSharpAsyncResumeRoute route in asyncRoutes)
        {
            if (route.CallbackId <= 0
                || route.PayloadKind is not (
                    SemanticContinuationCallback.NonePayloadKind or
                    SemanticContinuationCallback.ObjectPayloadKind)
                || !loweredFunctionIds.Contains(route.FunctionId))
            {
                Add(diagnostics, $"Async continuation callback '{route.CallbackId}' has no compatible lowered resume function.");
                return null;
            }
        }

        DispatchTarget[] targets = callbacks
            .Select(callback => new DispatchTarget(
                callback.CallbackId,
                callback.PayloadKind,
                CSharpGuestIds.Function(callback.MethodSymbolId)))
            .Concat(asyncRoutes.Select(route => new DispatchTarget(
                route.CallbackId,
                route.PayloadKind,
                route.FunctionId)))
            .OrderBy(target => target.CallbackId)
            .ToArray();
        if (targets.GroupBy(target => target.CallbackId).Any(group => group.Count() != 1))
        {
            Add(diagnostics, "Continuation callback identities overlap between explicit and compiler-generated routes.");
            return null;
        }

        GuestRegister callbackId = Parameter(version2, "callback_id", int32Type.Id);
        GuestRegister token = Parameter(version2, "token", int64Type.Id);
        GuestRegister status = Parameter(version2, "status", int32Type.Id);
        GuestRegister? objectSlot = version2
            ? Parameter(version2, "object_slot", int32Type.Id)
            : null;
        GuestRegister? objectGeneration = version2
            ? Parameter(version2, "object_generation", int32Type.Id)
            : null;
        List<GuestRegister> parameters = new() { callbackId, token, status };
        if (version2)
        {
            parameters.Add(objectSlot!);
            parameters.Add(objectGeneration!);
        }

        List<GuestRegister> locals = new();
        List<GuestBasicBlock> blocks = new();
        for (int index = 0; index < targets.Length; ++index)
        {
            DispatchTarget target = targets[index];
            string checkBlockId = CSharpGuestIds.ContinuationCheckBlock(version2, target.CallbackId);
            string callBlockId = CSharpGuestIds.ContinuationCallBlock(version2, target.CallbackId);
            string falseBlockId = index + 1 < targets.Length
                ? CSharpGuestIds.ContinuationCheckBlock(version2, targets[index + 1].CallbackId)
                : CSharpGuestIds.ContinuationReturnBlock(version2);
            GuestRegister callbackConstant = Local(
                version2,
                target.CallbackId,
                "callback_id_constant",
                int32Type.Id,
                locals);
            GuestRegister condition = Local(
                version2,
                target.CallbackId,
                "callback_id_matches",
                int32Type.Id,
                locals);
            blocks.Add(new GuestBasicBlock(
                checkBlockId,
                new GuestInstruction[]
                {
                    new(
                        "constant",
                        callbackConstant.Id,
                        Array.Empty<string>(),
                        null,
                        null,
                        new GuestConstant(
                            "int32",
                            target.CallbackId.ToString(CultureInfo.InvariantCulture))),
                    new(
                        "binary",
                        condition.Id,
                        new[] { callbackId.Id, callbackConstant.Id },
                        null,
                        "equals",
                        null),
                },
                new GuestTerminator("branch_if", condition.Id, callBlockId, falseBlockId, null)));

            List<GuestInstruction> callInstructions = new();
            string[] argumentIds = Array.Empty<string>();
            if (target.PayloadKind == SemanticContinuationCallback.ObjectPayloadKind)
            {
                GuestRegister callbackStatus = Local(
                    version2,
                    target.CallbackId,
                    "status",
                    continuationStatusType!.Id,
                    locals);
                callInstructions.Add(new GuestInstruction(
                    "convert",
                    callbackStatus.Id,
                    new[] { status.Id },
                    null,
                    null,
                    null));
                GuestRegister loadedObject = Local(
                    version2,
                    target.CallbackId,
                    "loaded_object",
                    loadedObjectType!.Id,
                    locals);
                callInstructions.Add(new GuestInstruction(
                    "stack_alloc",
                    loadedObject.Id,
                    Array.Empty<string>(),
                    null,
                    null,
                    null));
                Store(
                    loadedObject,
                    loadedObjectType,
                    "Slot",
                    objectSlot!,
                    callInstructions);
                Store(
                    loadedObject,
                    loadedObjectType,
                    "Generation",
                    objectGeneration!,
                    callInstructions);
                argumentIds = new[] { callbackStatus.Id, loadedObject.Id };
            }

            callInstructions.Add(new GuestInstruction(
                "call",
                null,
                argumentIds,
                target.FunctionId,
                null,
                null));
            blocks.Add(new GuestBasicBlock(
                callBlockId,
                callInstructions,
                Return()));
        }

        blocks.Add(new GuestBasicBlock(
            CSharpGuestIds.ContinuationReturnBlock(version2),
            Array.Empty<GuestInstruction>(),
            Return()));
        GuestFunction function = new(
            functionId,
            parameters,
            locals,
            voidType.Id,
            CSharpGuestIds.ContinuationCheckBlock(version2, targets[0].CallbackId),
            blocks);
        return new CSharpContinuationLoweringResult(
            function,
            new GuestExport(exportName, function.Id));
    }

    private static bool HasCompatibleParameters(
        SemanticContinuationCallback callback,
        SemanticCallable callable,
        bool version2)
    {
        SemanticCallableParameter[] parameters = callable.Parameters
            .OrderBy(parameter => parameter.Ordinal)
            .ToArray();
        return callback.PayloadKind switch
        {
            SemanticContinuationCallback.NonePayloadKind => parameters.Length == 0,
            SemanticContinuationCallback.ObjectPayloadKind => version2
                && parameters.Length == 2
                && parameters[0].Ordinal == 0
                && parameters[0].RefKind == "none"
                && parameters[0].TypeId == CSharpGuestIds.ContinuationStatusTypeId
                && parameters[1].Ordinal == 1
                && parameters[1].RefKind == "none"
                && parameters[1].TypeId == CSharpGuestIds.LoadedObjectTypeId,
            _ => false,
        };
    }

    private static bool TryGetObjectPayloadTypes(
        IReadOnlyDictionary<string, GuestType> guestTypes,
        string int32TypeId,
        out GuestType continuationStatusType,
        out GuestType loadedObjectType)
    {
        if (!guestTypes.TryGetValue(
                CSharpGuestIds.ContinuationStatusTypeId,
                out continuationStatusType!)
            || continuationStatusType.Kind != "enum"
            || continuationStatusType.Storage != "i32"
            || continuationStatusType.UnderlyingTypeId != int32TypeId
            || !guestTypes.TryGetValue(CSharpGuestIds.LoadedObjectTypeId, out loadedObjectType!)
            || loadedObjectType.Kind != "struct"
            || loadedObjectType.Fields.Count != 2
            || !HasField(loadedObjectType, "Slot", int32TypeId)
            || !HasField(loadedObjectType, "Generation", int32TypeId))
        {
            continuationStatusType = null!;
            loadedObjectType = null!;
            return false;
        }

        return true;
    }

    private static void Store(
        GuestRegister aggregate,
        GuestType aggregateType,
        string fieldName,
        GuestRegister value,
        ICollection<GuestInstruction> instructions)
    {
        instructions.Add(new GuestInstruction(
            "field_store",
            null,
            new[] { aggregate.Id, value.Id },
            FindField(aggregateType, fieldName)!.Id,
            null,
            null));
    }

    private static bool HasField(GuestType type, string name, string typeId)
    {
        return FindField(type, name) is { } field
            && field.TypeId == typeId;
    }

    private static GuestField? FindField(GuestType type, string name)
    {
        GuestField? result = null;
        foreach (GuestField field in type.Fields.Where(field => field.Name == name))
        {
            if (result is not null)
            {
                return null;
            }

            result = field;
        }

        return result;
    }

    private static GuestRegister Parameter(bool version2, string name, string typeId)
    {
        return new GuestRegister(CSharpGuestIds.ContinuationParameter(version2, name), typeId);
    }

    private static GuestRegister Local(
        bool version2,
        int callbackId,
        string name,
        string typeId,
        ICollection<GuestRegister> locals)
    {
        GuestRegister value = new(
            CSharpGuestIds.ContinuationLocal(version2, callbackId, name),
            typeId);
        locals.Add(value);
        return value;
    }

    private static GuestTerminator Return()
    {
        return new GuestTerminator("return", null, null, null, null);
    }

    private static void Add(List<GuestDiagnostic> diagnostics, string message)
    {
        diagnostics.Add(new GuestDiagnostic("ASCG1009", "error", message, null));
    }
}
