using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal sealed record CSharpAsyncResumeRoute(
    int CallbackId,
    string PayloadKind,
    string FunctionId);

internal sealed record CSharpAsyncLoweringResult(
    IReadOnlyList<GuestFunction> Functions,
    IReadOnlyList<CSharpAsyncResumeRoute> ResumeRoutes);

internal static class CSharpAsyncLowerer
{
    public static CSharpAsyncLoweringResult Lower(
        SemanticDocument document,
        IReadOnlyDictionary<string, GuestType> guestTypes,
        CSharpGuestDataPool dataPool,
        List<GuestDiagnostic> diagnostics)
    {
        if (document.AsyncMethods.Count == 0)
        {
            return new CSharpAsyncLoweringResult(
                Array.Empty<GuestFunction>(),
                Array.Empty<CSharpAsyncResumeRoute>());
        }

        Dictionary<string, SemanticCallable> callables = document.Callables
            .GroupBy(callable => callable.MethodSymbolId, StringComparer.Ordinal)
            .ToDictionary(group => group.Key, group => group.Single(), StringComparer.Ordinal);
        string? delayImportId = FindImport(document, "env", "continuation_delay");
        string? objectLoadImportId = FindImport(document, "env", "continuation_load_object");
        string? bindCancellationImportId = FindImport(
            document,
            "env",
            "continuation_bind_cancel");
        bool needsDelayImport = document.AsyncMethods
            .SelectMany(method => method.Segments)
            .Any(segment => segment.AwaitSite?.ProducerKind is "delay" or "next_tick");
        bool needsObjectLoadImport = document.AsyncMethods
            .SelectMany(method => method.Segments)
            .Any(segment => segment.AwaitSite?.ProducerKind == "object_load");
        bool needsBindCancellationImport = document.AsyncMethods
            .SelectMany(method => method.Segments)
            .Any(segment => segment.AwaitSite?.CancellationToken is not null);
        if ((needsDelayImport && delayImportId is null)
            || (needsObjectLoadImport && objectLoadImportId is null)
            || (needsBindCancellationImport && bindCancellationImportId is null))
        {
            Add(diagnostics, "The async profile is missing a required built-in continuation import.");
            return new CSharpAsyncLoweringResult(
                Array.Empty<GuestFunction>(),
                Array.Empty<CSharpAsyncResumeRoute>());
        }

        if (!guestTypes.TryGetValue("type:void", out GuestType? voidType)
            || !guestTypes.TryGetValue(CSharpGuestIds.Int32TypeId, out GuestType? int32Type)
            || !guestTypes.TryGetValue(CSharpGuestIds.Int64TypeId, out GuestType? int64Type)
            || !guestTypes.TryGetValue(CSharpGuestIds.ContinuationStatusTypeId, out GuestType? statusType)
            || !guestTypes.TryGetValue(CSharpGuestIds.LoadedObjectTypeId, out GuestType? loadedObjectType))
        {
            Add(diagnostics, "The async profile requires canonical void, integer, status, and loaded-object types.");
            return new CSharpAsyncLoweringResult(
                Array.Empty<GuestFunction>(),
                Array.Empty<CSharpAsyncResumeRoute>());
        }

        List<GuestFunction> functions = new();
        List<CSharpAsyncResumeRoute> routes = new();
        foreach (SemanticAsyncMethod method in document.AsyncMethods
            .OrderBy(item => item.MethodSymbolId, StringComparer.Ordinal))
        {
            if (!callables.TryGetValue(method.MethodSymbolId, out SemanticCallable? callable)
                || callable.Parameters.Count != 0
                || !callable.IsStatic
                || callable.ReturnTypeId != voidType.Id
                || callable.Export?.Name != method.ExportName
                || method.Segments.Count == 0)
            {
                Add(diagnostics, $"Async method '{method.MethodSymbolId}' has no compatible exported callable.");
                continue;
            }

            for (int index = 0; index < method.Segments.Count; ++index)
            {
                SemanticAsyncSegment segment = method.Segments[index];
                SemanticAsyncAwaitSite? incoming = index == 0
                    ? null
                    : method.Segments[index - 1].AwaitSite;
                if (index > 0 && incoming is null)
                {
                    Add(diagnostics, $"Async method '{method.MethodSymbolId}' has a disconnected segment {segment.Ordinal}.");
                    break;
                }

                List<GuestRegister> parameters = new();
                GuestRegister? loadedObjectParameter = null;
                if (incoming?.PayloadKind == SemanticContinuationCallback.ObjectPayloadKind)
                {
                    parameters.Add(new GuestRegister(
                        CSharpGuestIds.AsyncResumeParameter(incoming.CallbackId, "status"),
                        statusType.Id));
                    loadedObjectParameter = new GuestRegister(
                        CSharpGuestIds.AsyncResumeParameter(incoming.CallbackId, "loaded_object"),
                        loadedObjectType.Id);
                    parameters.Add(loadedObjectParameter);
                }

                CSharpFunctionLoweringContext context = new(
                    document,
                    callable,
                    guestTypes,
                    dataPool,
                    Array.Empty<GuestRegister>(),
                    diagnostics);
                if (incoming?.ResultSymbolId is not null
                    && (loadedObjectParameter is null
                        || !context.TryBindStorage(incoming.ResultSymbolId, loadedObjectParameter)))
                {
                    Add(diagnostics, $"Async object result '{incoming.ResultSymbolId}' has no compatible resume storage.");
                    break;
                }

                List<GuestInstruction> instructions = new();
                foreach (SemanticAsyncStatement statement in segment.Statements)
                {
                    GuestRegister? value = CSharpOperationLowerer.LowerValue(
                        context,
                        statement.Operation,
                        segment.Ordinal,
                        instructions);
                    if (statement.TargetSymbolId is not null
                        && (value is null
                            || !CSharpOperationLowerer.StoreLocal(
                                context,
                                statement.TargetSymbolId,
                                value,
                                segment.Ordinal,
                                instructions)))
                    {
                        break;
                    }
                }

                GuestRegister? scheduledToken = null;
                GuestRegister? cancellationBindingAccepted = null;
                if (segment.AwaitSite is { } awaitSite)
                {
                    if (!EmitProducer(
                        context,
                        awaitSite,
                        delayImportId,
                        objectLoadImportId,
                        bindCancellationImportId,
                        int32Type,
                        int64Type,
                        instructions,
                        out scheduledToken,
                        out cancellationBindingAccepted))
                    {
                        break;
                    }

                    routes.Add(new CSharpAsyncResumeRoute(
                        awaitSite.CallbackId,
                        awaitSite.PayloadKind,
                        CSharpGuestIds.AsyncResumeFunction(awaitSite.CallbackId)));
                }

                string functionId = index == 0
                    ? CSharpGuestIds.Function(method.MethodSymbolId)
                    : CSharpGuestIds.AsyncResumeFunction(incoming!.CallbackId);
                string blockId = CSharpGuestIds.AsyncSegmentBlock(
                    method.MethodSymbolId,
                    segment.Ordinal);
                IReadOnlyList<GuestBasicBlock> blocks;
                if (scheduledToken is not null)
                {
                    GuestRegister? zeroToken = context.CreateTemporary(
                        int64Type.Id,
                        segment.Ordinal);
                    GuestRegister? scheduleAccepted = context.CreateTemporary(
                        int32Type.Id,
                        segment.Ordinal);
                    if (zeroToken is null || scheduleAccepted is null)
                    {
                        break;
                    }

                    instructions.Add(new GuestInstruction(
                        "constant",
                        zeroToken.Id,
                        Array.Empty<string>(),
                        null,
                        null,
                        new GuestConstant("int64", "0")));
                    instructions.Add(new GuestInstruction(
                        "binary",
                        scheduleAccepted.Id,
                        new[] { scheduledToken.Id, zeroToken.Id },
                        null,
                        "not_equals",
                        null));
                    GuestRegister finalAcceptance = scheduleAccepted;
                    if (cancellationBindingAccepted is not null)
                    {
                        GuestRegister? combinedAcceptance = context.CreateTemporary(
                            int32Type.Id,
                            segment.Ordinal);
                        if (combinedAcceptance is null)
                        {
                            break;
                        }
                        instructions.Add(new GuestInstruction(
                            "binary",
                            combinedAcceptance.Id,
                            new[]
                            {
                                scheduleAccepted.Id,
                                cancellationBindingAccepted.Id,
                            },
                            null,
                            "bitwise_and",
                            null));
                        finalAcceptance = combinedAcceptance;
                    }
                    string acceptedBlockId = blockId + ":schedule_accepted";
                    string rejectedBlockId = blockId + ":schedule_rejected";
                    blocks = new[]
                    {
                        new GuestBasicBlock(
                            blockId,
                            instructions,
                            new GuestTerminator(
                                "branch_if",
                                finalAcceptance.Id,
                                acceptedBlockId,
                                rejectedBlockId,
                                null)),
                        new GuestBasicBlock(
                            acceptedBlockId,
                            Array.Empty<GuestInstruction>(),
                            new GuestTerminator("return", null, null, null, null)),
                        new GuestBasicBlock(
                            rejectedBlockId,
                            Array.Empty<GuestInstruction>(),
                            new GuestTerminator("trap", null, null, null, null)),
                    };
                }
                else
                {
                    blocks = new[]
                    {
                        new GuestBasicBlock(
                            blockId,
                            instructions,
                            new GuestTerminator("return", null, null, null, null)),
                    };
                }

                functions.Add(new GuestFunction(
                    functionId,
                    parameters,
                    context.Locals,
                    voidType.Id,
                    blockId,
                    blocks));
            }
        }

        if (routes.GroupBy(route => route.CallbackId).Any(group => group.Count() != 1))
        {
            Add(diagnostics, "Async resume callback ids are duplicated.");
        }

        return new CSharpAsyncLoweringResult(functions, routes);
    }

    private static bool EmitProducer(
        CSharpFunctionLoweringContext context,
        SemanticAsyncAwaitSite awaitSite,
        string? delayImportId,
        string? objectLoadImportId,
        string? bindCancellationImportId,
        GuestType int32Type,
        GuestType int64Type,
        List<GuestInstruction> instructions,
        out GuestRegister? scheduledToken,
        out GuestRegister? cancellationBindingAccepted)
    {
        scheduledToken = null;
        cancellationBindingAccepted = null;
        List<string> operands = new();
        string targetId;
        switch (awaitSite.ProducerKind)
        {
            case "delay":
                if (awaitSite.Arguments.Count != 1 || delayImportId is null)
                {
                    Add(context.Diagnostics, "DelayAsync await site must contain one delay argument.");
                    return false;
                }

                GuestRegister? delay = CSharpOperationLowerer.LowerValue(
                    context,
                    awaitSite.Arguments[0],
                    awaitSite.CallbackId,
                    instructions);
                if (delay is null)
                {
                    return false;
                }
                operands.Add(delay.Id);
                targetId = delayImportId;
                break;
            case "next_tick":
                if (delayImportId is null
                    || awaitSite.Arguments.Count != 0
                    || !context.TryGetGuestType("type:float32", out GuestType floatType))
                {
                    Add(context.Diagnostics, "NextTickAsync await site has an invalid delay ABI.");
                    return false;
                }

                GuestRegister? zero = context.CreateTemporary(floatType.Id, awaitSite.CallbackId);
                if (zero is null)
                {
                    return false;
                }
                instructions.Add(new GuestInstruction(
                    "constant",
                    zero.Id,
                    Array.Empty<string>(),
                    null,
                    null,
                    new GuestConstant("float32", "0")));
                operands.Add(zero.Id);
                targetId = delayImportId;
                break;
            case "object_load":
                if (awaitSite.Arguments.Count != 1 || objectLoadImportId is null)
                {
                    Add(context.Diagnostics, "LoadObjectAsync await site must contain one asset-path argument.");
                    return false;
                }

                GuestRegister? path = CSharpOperationLowerer.LowerValue(
                    context,
                    awaitSite.Arguments[0],
                    awaitSite.CallbackId,
                    instructions);
                if (path is null)
                {
                    return false;
                }
                operands.Add(path.Id);
                targetId = objectLoadImportId;
                break;
            default:
				if (!awaitSite.ProducerKind.StartsWith(
						"binding_latent|",
						StringComparison.Ordinal))
				{
					Add(context.Diagnostics, $"Unknown async producer kind '{awaitSite.ProducerKind}'.");
					return false;
				}
				string[] identity = awaitSite.ProducerKind.Split('|');
                SemanticCallable? latentImport = identity.Length == 3
                    ? FindImportCallable(context.Document, identity[1], identity[2])
					: null;
				if (identity.Length != 3
					|| latentImport is null)
				{
					Add(context.Diagnostics, $"Latent async producer '{awaitSite.ProducerKind}' has no unique import.");
					return false;
				}
				targetId = CSharpGuestIds.Import(latentImport.MethodSymbolId);
				if (!CSharpLatentStoragePlanner.TryBuild(
						context.Document,
						awaitSite.Arguments,
						latentImport.Parameters.Take(latentImport.Parameters.Count - 1).ToArray(),
						out CSharpLatentStoragePlan? storagePlan))
				{
					Add(context.Diagnostics, $"Latent async producer '{awaitSite.ProducerKind}' has no valid storage plan.");
					return false;
				}
				for (int index = 0; index < awaitSite.Arguments.Count; ++index)
				{
					if (!LowerLatentArgument(
						context,
						awaitSite.Arguments[index],
						storagePlan.Arguments[index],
						awaitSite.CallbackId,
						instructions,
						operands))
					{
						return false;
					}
                }
				break;
        }

        string? cancellationOperand = null;
        if (awaitSite.CancellationToken is { } cancellationToken)
        {
            if (bindCancellationImportId is null
                || !CSharpLatentStoragePlanner.TryBuildSingleValue(
                    context.Document,
                    cancellationToken,
                    CSharpGuestIds.Int64TypeId,
                    out CSharpLatentStorageArgumentPlan? cancellationPlan))
            {
                Add(context.Diagnostics, "Cancellation token has no valid i64 storage plan.");
                return false;
            }

            List<string> cancellationOperands = new();
            if (!LowerLatentArgument(
                    context,
                    cancellationToken,
                    cancellationPlan,
                    awaitSite.CallbackId,
                    instructions,
                    cancellationOperands)
                || cancellationOperands.Count != 1)
            {
                return false;
            }
            cancellationOperand = cancellationOperands[0];
        }

        GuestRegister? callback = context.CreateTemporary(int32Type.Id, awaitSite.CallbackId);
        GuestRegister? token = context.CreateTemporary(int64Type.Id, awaitSite.CallbackId);
        if (callback is null || token is null)
        {
            return false;
        }

        instructions.Add(new GuestInstruction(
            "constant",
            callback.Id,
            Array.Empty<string>(),
            null,
            null,
            new GuestConstant(
                "int32",
                awaitSite.CallbackId.ToString(CultureInfo.InvariantCulture))));
        operands.Add(callback.Id);
        instructions.Add(new GuestInstruction(
            "call",
            token.Id,
            operands,
            targetId,
            null,
            null));
        scheduledToken = token;

        if (cancellationOperand is not null)
        {
            GuestRegister? bindingAccepted = context.CreateTemporary(
                int32Type.Id,
                awaitSite.CallbackId);
            if (bindingAccepted is null)
            {
                return false;
            }
            instructions.Add(new GuestInstruction(
                "call",
                bindingAccepted.Id,
                new[] { cancellationOperand, token.Id },
                bindCancellationImportId!,
                null,
                null));
            cancellationBindingAccepted = bindingAccepted;
        }
        return true;
    }

    private static string? FindImport(
        SemanticDocument document,
        string module,
        string name)
    {
        SemanticCallable? callable = FindImportCallable(document, module, name);
        return callable is null
            ? null
            : CSharpGuestIds.Import(callable.MethodSymbolId);
    }

    private static SemanticCallable? FindImportCallable(
        SemanticDocument document,
        string module,
        string name)
    {
        SemanticCallable[] matches = document.Callables
            .Where(callable => callable.Import is { } import
                && import.Module == module
                && import.Name == name)
            .ToArray();
        return matches.Length == 1
            ? matches[0]
            : null;
    }

    private static bool LowerLatentArgument(
        CSharpFunctionLoweringContext context,
        SemanticOperation argument,
        CSharpLatentStorageArgumentPlan plan,
        int blockOrdinal,
        List<GuestInstruction> instructions,
        List<string> operands)
    {
        GuestRegister? root = CSharpOperationLowerer.LowerValue(
            context,
            argument,
            blockOrdinal,
            instructions);
        if (root is null)
        {
            return false;
        }

        foreach (CSharpLatentStorageCell cell in plan.Cells)
        {
            GuestRegister? value = cell.Kind switch
            {
                CSharpLatentStorageCellKind.Direct => root,
                CSharpLatentStorageCellKind.Field =>
                    CSharpAggregateOperationLowerer.LowerFieldPath(
                        context,
                        root,
                        cell.FieldPath,
                        blockOrdinal,
                        instructions),
                CSharpLatentStorageCellKind.Address =>
                    CSharpAggregateOperationLowerer.LowerStorageAddress(
                        context,
                        root,
                        blockOrdinal,
                        instructions),
                _ => null,
            };
            if (value is null)
            {
                return false;
            }
            GuestRegister? storage = AdaptLatentStorage(
                context,
                value,
                cell,
                blockOrdinal,
                instructions);
            if (storage is null)
            {
                return false;
            }
            operands.Add(storage.Id);
        }
        return true;
    }

    private static GuestRegister? AdaptLatentStorage(
        CSharpFunctionLoweringContext context,
        GuestRegister value,
        CSharpLatentStorageCell cell,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        if (value.TypeId != cell.SourceTypeId)
        {
            Add(context.Diagnostics,
                $"Latent storage plan expected '{cell.SourceTypeId}' but lowered '{value.TypeId}'.");
            return null;
        }
        if (value.TypeId == cell.StorageTypeId)
        {
            return value;
        }
        GuestRegister? storage = context.CreateTemporary(cell.StorageTypeId, blockOrdinal);
        if (storage is null)
        {
            return null;
        }
        instructions.Add(new GuestInstruction(
            "convert", storage.Id, new[] { value.Id }, null, null, null));
        return storage;
    }

    private static void Add(List<GuestDiagnostic> diagnostics, string message)
    {
        diagnostics.Add(new GuestDiagnostic("ASCG1010", "error", message, null));
    }
}
