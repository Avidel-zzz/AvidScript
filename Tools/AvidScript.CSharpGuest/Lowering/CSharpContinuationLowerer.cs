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
    public static CSharpContinuationLoweringResult? Lower(
        SemanticDocument document,
        IReadOnlyDictionary<string, GuestType> guestTypes,
        IReadOnlyList<GuestFunction> loweredFunctions,
        List<GuestDiagnostic> diagnostics)
    {
        if (document.ContinuationCallbacks.Count == 0)
        {
            return null;
        }

        if (document.Callables.Any(callable => string.Equals(
            callable.Export?.Name,
            CSharpGuestIds.ContinuationExportName,
            StringComparison.Ordinal)))
        {
            Add(diagnostics, "The compiler-generated continuation router conflicts with an explicit avid_on_continuation export.");
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
        foreach (SemanticContinuationCallback callback in callbacks)
        {
            if (!callablesById.TryGetValue(callback.MethodSymbolId, out SemanticCallable? callable)
                || callable.Parameters.Count != 0
                || !loweredFunctionIds.Contains(CSharpGuestIds.Function(callback.MethodSymbolId)))
            {
                Add(diagnostics, $"Continuation callback '{callback.CallbackId}' has no compatible lowered function.");
                return null;
            }
        }

        GuestRegister callbackId = Parameter("callback_id", int32Type.Id);
        GuestRegister token = Parameter("token", int64Type.Id);
        GuestRegister status = Parameter("status", int32Type.Id);
        List<GuestRegister> locals = new();
        List<GuestBasicBlock> blocks = new();
        for (int index = 0; index < callbacks.Length; ++index)
        {
            SemanticContinuationCallback callback = callbacks[index];
            string checkBlockId = CSharpGuestIds.ContinuationCheckBlock(callback.CallbackId);
            string callBlockId = CSharpGuestIds.ContinuationCallBlock(callback.CallbackId);
            string falseBlockId = index + 1 < callbacks.Length
                ? CSharpGuestIds.ContinuationCheckBlock(callbacks[index + 1].CallbackId)
                : CSharpGuestIds.ContinuationReturnBlockId;
            GuestRegister callbackConstant = Local(
                callback.CallbackId,
                "callback_id_constant",
                int32Type.Id,
                locals);
            GuestRegister condition = Local(
                callback.CallbackId,
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
                            callback.CallbackId.ToString(CultureInfo.InvariantCulture))),
                    new(
                        "binary",
                        condition.Id,
                        new[] { callbackId.Id, callbackConstant.Id },
                        null,
                        "equals",
                        null),
                },
                new GuestTerminator("branch_if", condition.Id, callBlockId, falseBlockId, null)));
            blocks.Add(new GuestBasicBlock(
                callBlockId,
                new[]
                {
                    new GuestInstruction(
                        "call",
                        null,
                        Array.Empty<string>(),
                        CSharpGuestIds.Function(callback.MethodSymbolId),
                        null,
                        null),
                },
                Return()));
        }

        blocks.Add(new GuestBasicBlock(
            CSharpGuestIds.ContinuationReturnBlockId,
            Array.Empty<GuestInstruction>(),
            Return()));
        GuestFunction function = new(
            CSharpGuestIds.ContinuationFunctionId,
            new[] { callbackId, token, status },
            locals,
            voidType.Id,
            CSharpGuestIds.ContinuationCheckBlock(callbacks[0].CallbackId),
            blocks);
        return new CSharpContinuationLoweringResult(
            function,
            new GuestExport(CSharpGuestIds.ContinuationExportName, function.Id));
    }

    private static GuestRegister Parameter(string name, string typeId)
    {
        return new GuestRegister(CSharpGuestIds.ContinuationParameter(name), typeId);
    }

    private static GuestRegister Local(
        int callbackId,
        string name,
        string typeId,
        ICollection<GuestRegister> locals)
    {
        GuestRegister value = new(CSharpGuestIds.ContinuationLocal(callbackId, name), typeId);
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
