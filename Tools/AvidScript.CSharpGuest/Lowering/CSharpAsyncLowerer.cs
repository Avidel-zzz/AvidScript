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
        string? resultReadImportId = FindImport(
            document,
            "env",
            "continuation_result_read");
        string? stateStoreImportId = FindImport(
            document,
            "env",
            "continuation_state_store");
        string? stateReadImportId = FindImport(
            document,
            "env",
            "continuation_state_read");
        string? cancelImportId = FindImport(document, "env", "continuation_cancel");
        bool needsDelayImport = document.AsyncMethods
            .SelectMany(method => method.Segments)
            .Any(segment => segment.AwaitSite?.ProducerKind is "delay" or "next_tick");
        bool needsObjectLoadImport = document.AsyncMethods
            .SelectMany(method => method.Segments)
            .Any(segment => segment.AwaitSite?.ProducerKind == "object_load");
        bool needsBindCancellationImport = document.AsyncMethods
            .SelectMany(method => method.Segments)
            .Any(segment => segment.AwaitSite?.CancellationToken is not null);
        bool needsResultReadImport = document.AsyncMethods
            .SelectMany(method => method.Segments)
            .Any(segment => segment.AwaitSite?.PayloadKind
                == SemanticContinuationCallback.ResultSlotPayloadKind);
        bool needsStateImports = document.AsyncMethods
            .SelectMany(method => method.Segments)
            .Any(segment => segment.AwaitSite?.StateFrame is not null);
        if ((needsDelayImport && delayImportId is null)
            || (needsObjectLoadImport && objectLoadImportId is null)
            || (needsBindCancellationImport && bindCancellationImportId is null)
            || (needsResultReadImport && resultReadImportId is null)
            || (needsStateImports
                && (stateStoreImportId is null
                    || stateReadImportId is null
                    || cancelImportId is null)))
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

                string functionId = index == 0
                    ? CSharpGuestIds.Function(method.MethodSymbolId)
                    : CSharpGuestIds.AsyncResumeFunction(incoming!.CallbackId);
                string blockId = CSharpGuestIds.AsyncSegmentBlock(
                    method.MethodSymbolId,
                    segment.Ordinal);

                List<GuestRegister> parameters = new();
                GuestRegister? continuationTokenParameter = null;
                GuestRegister? loadedObjectParameter = null;
                GuestRegister? resultStatusParameter = null;
                GuestRegister? resultSlotParameter = null;
                GuestRegister? resultGenerationParameter = null;
                if (incoming is not null)
                {
                    continuationTokenParameter = new GuestRegister(
                        CSharpGuestIds.AsyncResumeParameter(incoming.CallbackId, "token"),
                        int64Type.Id);
                    parameters.Add(continuationTokenParameter);
                }
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
                else if (incoming?.PayloadKind
                    == SemanticContinuationCallback.ResultSlotPayloadKind)
                {
                    resultStatusParameter = new GuestRegister(
                        CSharpGuestIds.AsyncResumeParameter(incoming.CallbackId, "status"),
                        statusType.Id);
                    resultSlotParameter = new GuestRegister(
                        CSharpGuestIds.AsyncResumeParameter(incoming.CallbackId, "result_slot"),
                        int32Type.Id);
                    resultGenerationParameter = new GuestRegister(
                        CSharpGuestIds.AsyncResumeParameter(incoming.CallbackId, "result_generation"),
                        int32Type.Id);
                    parameters.Add(resultStatusParameter);
                    parameters.Add(resultSlotParameter);
                    parameters.Add(resultGenerationParameter);
                }

                CSharpFunctionLoweringContext context = new(
                    document,
                    callable,
                    guestTypes,
                    dataPool,
                    Array.Empty<GuestRegister>(),
                    diagnostics);
                if (incoming?.ResultSymbolId is not null
                    && incoming.PayloadKind == SemanticContinuationCallback.ObjectPayloadKind
                    && (loadedObjectParameter is null
                        || !context.TryBindStorage(incoming.ResultSymbolId, loadedObjectParameter)))
                {
                    Add(diagnostics, $"Async object result '{incoming.ResultSymbolId}' has no compatible resume storage.");
                    break;
                }

                List<GuestBasicBlock> prefixBlocks = new();
                List<GuestInstruction> instructions = new();
                string activeBlockId = blockId;
                string guardReturnBlockId = blockId + ":guard_return";
                int guardOrdinal = 0;
                GuestRegister? incomingResultAccepted = null;
                GuestRegister? incomingOutcome = null;
                if (incoming?.StateFrame is { } incomingFrame)
                {
                    if (!EmitIncomingState(
                        context,
                        incomingFrame,
                        incoming.CallbackId,
                        continuationTokenParameter,
                        stateReadImportId,
                        int32Type,
                        instructions,
                        out GuestRegister? stateReadAccepted,
                        out IReadOnlyList<GuestInstruction>? restoreInstructions))
                    {
                        break;
                    }
                    string stateAcceptedBlockId = blockId + ":state_accepted";
                    string stateRejectedBlockId = blockId + ":state_rejected";
                    prefixBlocks.Add(new GuestBasicBlock(
                        activeBlockId,
                        instructions.ToArray(),
                        new GuestTerminator(
                            "branch_if",
                            stateReadAccepted!.Id,
                            stateAcceptedBlockId,
                            stateRejectedBlockId,
                            null)));
                    prefixBlocks.Add(new GuestBasicBlock(
                        stateRejectedBlockId,
                        Array.Empty<GuestInstruction>(),
                        new GuestTerminator("trap", null, null, null, null)));
                    activeBlockId = stateAcceptedBlockId;
                    instructions = restoreInstructions!.ToList();
                }
                if (incoming?.PayloadKind
                    == SemanticContinuationCallback.ResultSlotPayloadKind
                    && !EmitIncomingResult(
                        context,
                        incoming,
                        resultReadImportId,
                        resultStatusParameter,
                        resultSlotParameter,
                        resultGenerationParameter,
                        int32Type,
                        instructions,
                        out incomingResultAccepted,
                        out incomingOutcome))
                {
                    break;
                }
                if (incoming?.PayloadKind
                        == SemanticContinuationCallback.ResultSlotPayloadKind
                    && incoming.ResultSymbolId is not null
                    && (incomingOutcome is null
                        || !context.TryBindStorage(incoming.ResultSymbolId, incomingOutcome)))
                {
                    Add(diagnostics, $"Async outcome result '{incoming.ResultSymbolId}' has no compatible resume storage.");
                    break;
                }
                if (incomingResultAccepted is not null)
                {
                    string resultAcceptedBlockId = blockId + ":result_accepted";
                    string resultRejectedBlockId = blockId + ":result_rejected";
                    prefixBlocks.Add(new GuestBasicBlock(
                        activeBlockId,
                        instructions.ToArray(),
                        new GuestTerminator(
                            "branch_if",
                            incomingResultAccepted.Id,
                            resultAcceptedBlockId,
                            resultRejectedBlockId,
                            null)));
                    prefixBlocks.Add(new GuestBasicBlock(
                        resultRejectedBlockId,
                        Array.Empty<GuestInstruction>(),
                        new GuestTerminator("trap", null, null, null, null)));
                    activeBlockId = resultAcceptedBlockId;
                    instructions = new List<GuestInstruction>();
                }
                foreach (SemanticAsyncStatement statement in segment.Statements)
                {
                    if (statement.Operation.Kind
                        == SemanticAsyncMethod.EarlyReturnGuardOperationKind)
                    {
                        if (!EmitEarlyReturnGuard(
                            context,
                            statement.Operation,
                            segment.Ordinal,
                            guardOrdinal++,
                            activeBlockId,
                            guardReturnBlockId,
                            instructions,
                            prefixBlocks,
                            out string? continueBlockId))
                        {
                            break;
                        }
                        activeBlockId = continueBlockId!;
                        instructions = new List<GuestInstruction>();
                        continue;
                    }

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
                GuestRegister? stateStoreAccepted = null;
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

                    if (awaitSite.StateFrame is { } outgoingFrame
                        && !EmitOutgoingState(
                            context,
                            outgoingFrame,
                            awaitSite.CallbackId,
                            scheduledToken,
                            stateStoreImportId,
                            int32Type,
                            instructions,
                            out stateStoreAccepted))
                    {
                        break;
                    }

                    routes.Add(new CSharpAsyncResumeRoute(
                        awaitSite.CallbackId,
                        awaitSite.PayloadKind,
                        CSharpGuestIds.AsyncResumeFunction(awaitSite.CallbackId)));
                }

                IReadOnlyList<GuestBasicBlock> tailBlocks;
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
                    foreach (GuestRegister acceptance in new[]
                    {
                        cancellationBindingAccepted,
                        stateStoreAccepted,
                    }.Where(value => value is not null).Cast<GuestRegister>())
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
                                finalAcceptance.Id,
                                acceptance.Id,
                            },
                            null,
                            "bitwise_and",
                            null));
                        finalAcceptance = combinedAcceptance;
                    }
                    string acceptedBlockId = activeBlockId + ":schedule_accepted";
                    string rejectedBlockId = activeBlockId + ":schedule_rejected";
                    List<GuestInstruction> rejectionInstructions = new();
                    if (stateStoreAccepted is not null)
                    {
                        GuestRegister? cancellationIgnored = context.CreateTemporary(
                            int32Type.Id,
                            segment.Ordinal);
                        if (cancellationIgnored is null)
                        {
                            break;
                        }
                        rejectionInstructions.Add(new GuestInstruction(
                            "call",
                            cancellationIgnored.Id,
                            new[] { scheduledToken.Id },
                            cancelImportId,
                            null,
                            null));
                    }
                    tailBlocks = new[]
                    {
                        new GuestBasicBlock(
                            activeBlockId,
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
                            rejectionInstructions,
                            new GuestTerminator("trap", null, null, null, null)),
                    };
                }
                else
                {
                    tailBlocks = new[]
                    {
                        new GuestBasicBlock(
                            activeBlockId,
                            instructions,
                            new GuestTerminator("return", null, null, null, null)),
                    };
                }

                List<GuestBasicBlock> blocks = prefixBlocks
                    .Concat(tailBlocks)
                    .ToList();
                if (guardOrdinal > 0)
                {
                    blocks.Add(new GuestBasicBlock(
                        guardReturnBlockId,
                        Array.Empty<GuestInstruction>(),
                        new GuestTerminator("return", null, null, null, null)));
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

    private static bool EmitIncomingState(
        CSharpFunctionLoweringContext context,
        SemanticAsyncStateFrame frame,
        int blockOrdinal,
        GuestRegister? continuationToken,
        string? stateReadImportId,
        GuestType int32Type,
        List<GuestInstruction> instructions,
        out GuestRegister? stateReadAccepted,
        out IReadOnlyList<GuestInstruction>? restoreInstructions)
    {
        stateReadAccepted = null;
        restoreInstructions = null;
        if (continuationToken is null
            || stateReadImportId is null
            || !TryGetStateFrameType(context, frame, out GuestType frameType))
        {
            Add(context.Diagnostics, $"Async state frame '{frame.TypeId}' has no compatible resume contract.");
            return false;
        }

        GuestRegister? frameStorage = context.CreateTemporary(
            frameType.Id,
            blockOrdinal);
        GuestRegister? byteCount = context.CreateTemporary(
            int32Type.Id,
            blockOrdinal);
        if (frameStorage is null || byteCount is null)
        {
            return false;
        }
        instructions.Add(new GuestInstruction(
            "stack_alloc",
            frameStorage.Id,
            Array.Empty<string>(),
            null,
            null,
            null));
        GuestRegister? frameAddress = CSharpAggregateOperationLowerer.LowerStorageAddress(
            context,
            frameStorage,
            blockOrdinal,
            instructions);
        stateReadAccepted = context.CreateTemporary(
            int32Type.Id,
            blockOrdinal);
        if (frameAddress is null || stateReadAccepted is null)
        {
            return false;
        }
        instructions.Add(new GuestInstruction(
            "constant",
            byteCount.Id,
            Array.Empty<string>(),
            null,
            null,
            new GuestConstant(
                "int32",
                frameType.Size.ToString(CultureInfo.InvariantCulture))));
        instructions.Add(new GuestInstruction(
            "call",
            stateReadAccepted.Id,
            new[] { continuationToken.Id, frameAddress.Id, byteCount.Id },
            stateReadImportId,
            null,
            null));

        List<GuestInstruction> restore = new();
        for (int index = 0; index < frame.Slots.Count; ++index)
        {
            SemanticAsyncStateSlot slot = frame.Slots[index];
            GuestField field = frameType.Fields[index];
            GuestRegister? value = context.CreateTemporary(
                slot.TypeId,
                blockOrdinal);
            if (value is null)
            {
                return false;
            }
            restore.Add(new GuestInstruction(
                "field_load",
                value.Id,
                new[] { frameStorage.Id },
                field.Id,
                null,
                null));
            if (!CSharpOperationLowerer.StoreLocal(
                context,
                slot.SymbolId,
                value,
                blockOrdinal,
                restore))
            {
                return false;
            }
        }
        restoreInstructions = restore;
        return true;
    }

    private static bool EmitOutgoingState(
        CSharpFunctionLoweringContext context,
        SemanticAsyncStateFrame frame,
        int blockOrdinal,
        GuestRegister? continuationToken,
        string? stateStoreImportId,
        GuestType int32Type,
        List<GuestInstruction> instructions,
        out GuestRegister? stateStoreAccepted)
    {
        stateStoreAccepted = null;
        if (continuationToken is null
            || stateStoreImportId is null
            || !TryGetStateFrameType(context, frame, out GuestType frameType))
        {
            Add(context.Diagnostics, $"Async state frame '{frame.TypeId}' has no compatible suspend contract.");
            return false;
        }

        GuestRegister? frameStorage = context.CreateTemporary(frameType.Id, blockOrdinal);
        GuestRegister? byteCount = context.CreateTemporary(int32Type.Id, blockOrdinal);
        if (frameStorage is null || byteCount is null)
        {
            return false;
        }
        instructions.Add(new GuestInstruction(
            "stack_alloc",
            frameStorage.Id,
            Array.Empty<string>(),
            null,
            null,
            null));
        for (int index = 0; index < frame.Slots.Count; ++index)
        {
            SemanticAsyncStateSlot slot = frame.Slots[index];
            if (!context.TryGetStorage(slot.SymbolId, out GuestRegister storage)
                || storage.TypeId != slot.TypeId)
            {
                Add(context.Diagnostics, $"Async state slot '{slot.SymbolId}' has no live suspend storage.");
                return false;
            }
            instructions.Add(new GuestInstruction(
                "field_store",
                null,
                new[] { frameStorage.Id, storage.Id },
                frameType.Fields[index].Id,
                null,
                null));
        }

        GuestRegister? frameAddress = CSharpAggregateOperationLowerer.LowerStorageAddress(
            context,
            frameStorage,
            blockOrdinal,
            instructions);
        stateStoreAccepted = context.CreateTemporary(int32Type.Id, blockOrdinal);
        if (frameAddress is null || stateStoreAccepted is null)
        {
            return false;
        }
        instructions.Add(new GuestInstruction(
            "constant",
            byteCount.Id,
            Array.Empty<string>(),
            null,
            null,
            new GuestConstant(
                "int32",
                frameType.Size.ToString(CultureInfo.InvariantCulture))));
        instructions.Add(new GuestInstruction(
            "call",
            stateStoreAccepted.Id,
            new[] { continuationToken.Id, frameAddress.Id, byteCount.Id },
            stateStoreImportId,
            null,
            null));
        return true;
    }

    private static bool TryGetStateFrameType(
        CSharpFunctionLoweringContext context,
        SemanticAsyncStateFrame frame,
        out GuestType frameType)
    {
        if (!context.TryGetGuestType(frame.TypeId, out frameType)
            || frameType.Kind != "struct"
            || frameType.Storage != "memory"
            || frameType.Size <= 0
            || frameType.Size > 4096
            || frameType.Fields.Count != frame.Slots.Count)
        {
            return false;
        }
        return frameType.Fields.Select(field => (field.Name, field.TypeId))
            .SequenceEqual(frame.Slots.Select(slot => (slot.SymbolId, slot.TypeId)));
    }

    private static bool EmitEarlyReturnGuard(
        CSharpFunctionLoweringContext context,
        SemanticOperation operation,
        int segmentOrdinal,
        int guardOrdinal,
        string blockId,
        string returnBlockId,
        List<GuestInstruction> instructions,
        List<GuestBasicBlock> blocks,
        out string? continueBlockId)
    {
        continueBlockId = null;
        if (operation.Children.Count != 1
            || operation.TypeId != "type:void"
            || operation.Children[0].TypeId != "type:bool")
        {
            Add(context.Diagnostics, "Async early-return guard has no compatible bool condition.");
            return false;
        }

        GuestRegister? condition = CSharpOperationLowerer.LowerValue(
            context,
            operation.Children[0],
            segmentOrdinal,
            instructions);
        if (condition is null
            || condition.TypeId != "type:bool"
            || !context.TryGetGuestType(condition.TypeId, out GuestType conditionType)
            || conditionType.Kind != "scalar"
            || conditionType.Storage != "i32"
            || conditionType.Size != 1
            || conditionType.Alignment != 1)
        {
            Add(context.Diagnostics, "Async early-return guard did not lower to the canonical bool storage.");
            return false;
        }

        continueBlockId = blockId + $":guard_{guardOrdinal}_continue";
        blocks.Add(new GuestBasicBlock(
            blockId,
            instructions.ToArray(),
            new GuestTerminator(
                "branch_if",
                condition.Id,
                returnBlockId,
                continueBlockId,
                null)));
        return true;
    }

    private static bool EmitIncomingResult(
        CSharpFunctionLoweringContext context,
        SemanticAsyncAwaitSite incoming,
        string? resultReadImportId,
        GuestRegister? status,
        GuestRegister? resultSlot,
        GuestRegister? resultGeneration,
        GuestType int32Type,
        List<GuestInstruction> instructions,
        out GuestRegister? resultReadAccepted,
        out GuestRegister? outcome)
    {
        resultReadAccepted = null;
        outcome = null;
        if (resultReadImportId is null
            || status is null
            || resultSlot is null
            || resultGeneration is null
            || incoming.BindingOrdinal < 0
            || incoming.ResultTypeId is null
            || incoming.PayloadValueTypeId is null
            || !context.TryGetGuestType(incoming.ResultTypeId, out GuestType outcomeType)
            || !context.TryGetGuestType(incoming.PayloadValueTypeId, out GuestType payloadType)
            || outcomeType.Kind != "struct"
            || outcomeType.Fields.SingleOrDefault(field =>
                field.Id == CSharpGuestIds.OutcomeStatusField(outcomeType.Id)) is not { } statusField
            || outcomeType.Fields.SingleOrDefault(field =>
                field.Id == CSharpGuestIds.OutcomeValueField(outcomeType.Id)) is not { } valueField
            || statusField.TypeId != status.TypeId
            || valueField.TypeId != payloadType.Id
            || payloadType.Size <= 0)
        {
            Add(context.Diagnostics, "Incoming provider result has no compatible frozen Guest layout.");
            return false;
        }

        outcome = context.CreateTemporary(outcomeType.Id, incoming.CallbackId);
        GuestRegister? payloadStorage = context.CreateTemporary(
            payloadType.Id,
            incoming.CallbackId);
        GuestRegister? bindingOrdinal = context.CreateTemporary(
            int32Type.Id,
            incoming.CallbackId);
        GuestRegister? byteCount = context.CreateTemporary(
            int32Type.Id,
            incoming.CallbackId);
        if (outcome is null
            || payloadStorage is null
            || bindingOrdinal is null
            || byteCount is null)
        {
            return false;
        }

        instructions.Add(new GuestInstruction(
            "stack_alloc",
            outcome.Id,
            Array.Empty<string>(),
            null,
            null,
            null));
        instructions.Add(new GuestInstruction(
            "field_store",
            null,
            new[] { outcome.Id, status.Id },
            statusField.Id,
            null,
            null));
        if (payloadType.Kind == "struct")
        {
            instructions.Add(new GuestInstruction(
                "stack_alloc",
                payloadStorage.Id,
                Array.Empty<string>(),
                null,
                null,
                null));
        }
        GuestRegister? payloadAddress =
            CSharpAggregateOperationLowerer.LowerStorageAddress(
                context,
                payloadStorage,
                incoming.CallbackId,
                instructions);
        resultReadAccepted = context.CreateTemporary(
            int32Type.Id,
            incoming.CallbackId);
        if (payloadAddress is null || resultReadAccepted is null)
        {
            return false;
        }
        instructions.Add(new GuestInstruction(
            "constant",
            bindingOrdinal.Id,
            Array.Empty<string>(),
            null,
            null,
            new GuestConstant(
                "int32",
                incoming.BindingOrdinal.ToString(CultureInfo.InvariantCulture))));
        instructions.Add(new GuestInstruction(
            "constant",
            byteCount.Id,
            Array.Empty<string>(),
            null,
            null,
            new GuestConstant(
                "int32",
                payloadType.Size.ToString(CultureInfo.InvariantCulture))));
        instructions.Add(new GuestInstruction(
            "call",
            resultReadAccepted.Id,
            new[]
            {
                bindingOrdinal.Id,
                resultSlot.Id,
                resultGeneration.Id,
                payloadAddress.Id,
                byteCount.Id,
            },
            resultReadImportId,
            null,
            null));

        GuestRegister payloadValue = payloadStorage;
        if (payloadType.Kind != "struct")
        {
            GuestRegister? loadedPayload = context.CreateTemporary(
                payloadType.Id,
                incoming.CallbackId);
            if (loadedPayload is null)
            {
                return false;
            }
            instructions.Add(new GuestInstruction(
                "local_load",
                loadedPayload.Id,
                Array.Empty<string>(),
                payloadStorage.Id,
                null,
                null));
            payloadValue = loadedPayload;
        }
        instructions.Add(new GuestInstruction(
            "field_store",
            null,
            new[] { outcome.Id, payloadValue.Id },
            valueField.Id,
            null,
            null));
        return true;
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
