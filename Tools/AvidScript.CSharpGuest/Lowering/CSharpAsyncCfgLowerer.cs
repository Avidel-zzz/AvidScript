using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal sealed record CSharpAsyncAbi(
    string? DelayImportId,
    string? ObjectLoadImportId,
    string? BindCancellationImportId,
    string? ResultReadImportId,
    string? StateStoreImportId,
    string? StateReadImportId,
    string? CancelImportId,
    string? TaskResultImportId,
    GuestType Int32Type,
    GuestType Int64Type,
    GuestType StatusType,
    GuestType LoadedObjectType,
    GuestType VoidType);

internal static class CSharpAsyncCfgLowerer
{
    public static void Lower(
        SemanticDocument document,
        SemanticAsyncMethod method,
        SemanticCallable callable,
        IReadOnlyDictionary<string, GuestType> guestTypes,
        CSharpGuestDataPool dataPool,
        CSharpAsyncAbi abi,
        ICollection<GuestFunction> functions,
        ICollection<CSharpAsyncResumeRoute> routes,
        List<GuestDiagnostic> diagnostics)
    {
        if (method.EntrySegmentOrdinal < 0
            || method.EntrySegmentOrdinal >= method.Segments.Count
            || method.Segments.Any(segment => segment.Transfer is null))
        {
            Add(diagnostics, method, "Continuation CFG has no valid entry or transfer graph.");
            return;
        }

        foreach (SemanticAsyncAwaitSite awaitSite in method.Segments
            .Where(segment => segment.AwaitSite is not null)
            .Select(segment => segment.AwaitSite!)
            .OrderBy(site => site.CallbackId))
        {
            routes.Add(new CSharpAsyncResumeRoute(
                awaitSite.CallbackId,
                awaitSite.PayloadKind,
                CSharpGuestIds.AsyncResumeFunction(awaitSite.CallbackId)));
        }

        List<EntryPoint> entries = new()
        {
            new EntryPoint(
                CSharpGuestIds.Function(method.MethodSymbolId),
                method.EntrySegmentOrdinal,
                null),
        };
        entries.AddRange(method.Segments
            .Where(segment => segment.AwaitSite is not null)
            .OrderBy(segment => segment.AwaitSite!.CallbackId)
            .Select(segment => new EntryPoint(
                CSharpGuestIds.AsyncResumeFunction(segment.AwaitSite!.CallbackId),
                segment.Transfer!.PrimaryTarget,
                segment.AwaitSite)));

        foreach (EntryPoint entry in entries)
        {
            if (!TryLowerEntry(
                document,
                method,
                callable,
                entry,
                guestTypes,
                dataPool,
                abi,
                diagnostics,
                out GuestFunction? function))
            {
                return;
            }
            functions.Add(function!);
        }
    }

    private static bool TryLowerEntry(
        SemanticDocument document,
        SemanticAsyncMethod method,
        SemanticCallable callable,
        EntryPoint entry,
        IReadOnlyDictionary<string, GuestType> guestTypes,
        CSharpGuestDataPool dataPool,
        CSharpAsyncAbi abi,
        List<GuestDiagnostic> diagnostics,
        out GuestFunction? function)
    {
        function = null;
        if (entry.SegmentOrdinal < 0 || entry.SegmentOrdinal >= method.Segments.Count)
        {
            Add(diagnostics, method, $"Continuation entry targets missing segment {entry.SegmentOrdinal}.");
            return false;
        }

        List<GuestRegister> parameters = new();
        GuestRegister? continuationToken = null;
        GuestRegister? loadedObject = null;
        GuestRegister? resultStatus = null;
        GuestRegister? resultSlot = null;
        GuestRegister? resultGeneration = null;
        GuestRegister? outcome = null;
        SemanticAsyncAwaitSite? incoming = entry.Incoming;
        if (incoming is not null)
        {
            continuationToken = new GuestRegister(
                CSharpGuestIds.AsyncResumeParameter(incoming.CallbackId, "token"),
                abi.Int64Type.Id);
            parameters.Add(continuationToken);
        }
        if (incoming?.PayloadKind == SemanticContinuationCallback.ObjectPayloadKind)
        {
            parameters.Add(new GuestRegister(
                CSharpGuestIds.AsyncResumeParameter(incoming.CallbackId, "status"),
                abi.StatusType.Id));
            loadedObject = new GuestRegister(
                CSharpGuestIds.AsyncResumeParameter(incoming.CallbackId, "loaded_object"),
                abi.LoadedObjectType.Id);
            parameters.Add(loadedObject);
        }
        else if (incoming?.PayloadKind == SemanticContinuationCallback.ResultSlotPayloadKind)
        {
            resultStatus = new GuestRegister(
                CSharpGuestIds.AsyncResumeParameter(incoming.CallbackId, "status"),
                abi.StatusType.Id);
            resultSlot = new GuestRegister(
                CSharpGuestIds.AsyncResumeParameter(incoming.CallbackId, "result_slot"),
                abi.Int32Type.Id);
            resultGeneration = new GuestRegister(
                CSharpGuestIds.AsyncResumeParameter(incoming.CallbackId, "result_generation"),
                abi.Int32Type.Id);
            parameters.Add(resultStatus);
            parameters.Add(resultSlot);
            parameters.Add(resultGeneration);
        }

        List<GuestRegister> invocation = new();
        if (!callable.IsStatic) invocation.Add(new(CSharpGuestIds.This(callable.MethodSymbolId), callable.ContainingTypeId));
        invocation.AddRange(callable.Parameters.OrderBy(parameter => parameter.Ordinal)
            .Select(parameter => new GuestRegister(CSharpGuestIds.Parameter(parameter.SymbolId), parameter.TypeId)));
        if (incoming is null) parameters.AddRange(invocation);
        CSharpFunctionLoweringContext context = new(
            document,
            callable,
            guestTypes,
            dataPool,
            invocation,
            diagnostics,
            method.CompilerLocals,
            invocationStorageIsLocal: incoming is not null);
        if (method.TaskResultTypeId is not null
            && context.CreateInternalStorage(CSharpTaskResultAbi.ProducerSlot(method),
                CSharpTaskResultAbi.TokenTypeId) is null)
            return false;
        foreach (SemanticAsyncAwaitSite taskAwait in method.Segments
            .Select(segment => segment.AwaitSite)
            .Where(site => site?.ProducerKind == "task_call").Cast<SemanticAsyncAwaitSite>())
            if (context.CreateInternalStorage(CSharpTaskResultAbi.AwaitSlot(taskAwait),
                CSharpTaskResultAbi.TokenTypeId) is null)
                return false;
        if (incoming?.ResultSymbolId is not null
            && incoming.PayloadKind == SemanticContinuationCallback.ObjectPayloadKind
            && (loadedObject is null
                || (!context.ClosureCells.Has(incoming.ResultSymbolId) && !context.TryBindStorage(incoming.ResultSymbolId, loadedObject))))
        {
            Add(diagnostics, method, $"Async object result '{incoming.ResultSymbolId}' has no compatible resume storage.");
            return false;
        }

        string firstFlowBlockId = FlowBlockId(method, entry.SegmentOrdinal);
        string functionEntryBlockId = firstFlowBlockId + ":entry";
        string activePrefixBlockId = functionEntryBlockId;
        List<GuestInstruction> prefixInstructions = new();
        List<GuestBasicBlock> blocks = new();
        if (incoming is null && method.TaskResultTypeId is not null)
        {
            if (abi.TaskResultImportId is null
                || !context.TryGetStorage(CSharpTaskResultAbi.ProducerSlot(method), out GuestRegister taskStorage))
                return false;
            GuestRegister? task = CSharpTaskResultAbi.Call(context,
                CSharpTaskResultAbi.Create, null, null, entry.SegmentOrdinal, prefixInstructions);
            if (task is null || CSharpTaskResultAbi.Call(context,
                    CSharpTaskResultAbi.Retain, task, null, entry.SegmentOrdinal, prefixInstructions) is null)
                return false;
            prefixInstructions.Add(new("local_store", null, new[] { task.Id }, taskStorage.Id, null, null));
        }
        if (incoming is not null && CSharpAsyncClosureState.Frame(document, method, incoming) is { } incomingFrame)
        {
            if (!CSharpAsyncLowerer.EmitIncomingState(
                context,
                incomingFrame,
                incoming.CallbackId,
                continuationToken,
                abi.StateReadImportId,
                abi.Int32Type,
                prefixInstructions,
                out GuestRegister? stateReadAccepted,
                out IReadOnlyList<GuestInstruction>? restoreInstructions))
            {
                return false;
            }
            string acceptedBlockId = functionEntryBlockId + ":state_accepted";
            string rejectedBlockId = functionEntryBlockId + ":state_rejected";
            blocks.Add(new GuestBasicBlock(
                activePrefixBlockId,
                prefixInstructions.ToArray(),
                new GuestTerminator(
                    "branch_if",
                    stateReadAccepted!.Id,
                    acceptedBlockId,
                    rejectedBlockId,
                    null)));
            blocks.Add(new GuestBasicBlock(
                rejectedBlockId,
                Array.Empty<GuestInstruction>(),
                new GuestTerminator("trap", null, null, null, null)));
            activePrefixBlockId = acceptedBlockId;
            prefixInstructions = restoreInstructions!.ToList();
        }

        if (incoming?.PayloadKind == SemanticContinuationCallback.ResultSlotPayloadKind)
        {
            if (!CSharpAsyncLowerer.EmitIncomingResult(
                context,
                incoming,
                abi.ResultReadImportId,
                resultStatus,
                resultSlot,
                resultGeneration,
                abi.Int32Type,
                prefixInstructions,
                out GuestRegister? resultReadAccepted,
                out outcome))
            {
                return false;
            }
            if (incoming.ResultSymbolId is not null
                && (outcome is null
                    || (!context.ClosureCells.Has(incoming.ResultSymbolId) && !context.TryBindStorage(incoming.ResultSymbolId, outcome))))
            {
                Add(diagnostics, method, $"Async outcome result '{incoming.ResultSymbolId}' has no compatible resume storage.");
                return false;
            }
            string acceptedBlockId = functionEntryBlockId + ":result_accepted";
            string rejectedBlockId = functionEntryBlockId + ":result_rejected";
            blocks.Add(new GuestBasicBlock(
                activePrefixBlockId,
                prefixInstructions.ToArray(),
                new GuestTerminator(
                    "branch_if",
                    resultReadAccepted!.Id,
                    acceptedBlockId,
                    rejectedBlockId,
                    null)));
            blocks.Add(new GuestBasicBlock(
                rejectedBlockId,
                Array.Empty<GuestInstruction>(),
                new GuestTerminator("trap", null, null, null, null)));
            activePrefixBlockId = acceptedBlockId;
            prefixInstructions = new List<GuestInstruction>();
        }
        if (incoming?.ProducerKind == "task_call")
        {
            if (!CSharpTaskAwaitLowerer.EmitIncoming(context, incoming,
                entry.SegmentOrdinal, prefixInstructions, blocks,
                ref activePrefixBlockId, out List<GuestInstruction>? resumedPrefix))
                return false;
            prefixInstructions = resumedPrefix!;
        }
        int? incomingSegment = incoming is null ? null : method.Segments.Single(segment => segment.AwaitSite?.CallbackId == incoming.CallbackId).Ordinal;
        if (context.ThisRegister is { } receiver)
        {
            // Guards run after restoration and before any resumed side effects.
            if (CSharpUeReceivers.IsType(document, receiver.TypeId))
                CSharpUeReceivers.Require(context, receiver, entry.SegmentOrdinal, prefixInstructions);
            else CSharpReferenceObjects.Require(receiver, prefixInstructions);
        }
        CSharpAsyncClosureAllocations.Transition(context, method, incomingSegment, entry.SegmentOrdinal, prefixInstructions);
        if (incoming?.ResultSymbolId is { } resultSymbol && context.ClosureCells.Has(resultSymbol))
        {
            GuestRegister? value = loadedObject ?? outcome;
            if (value is null || !CSharpOperationLowerer.StoreLocal(context, resultSymbol, value, entry.SegmentOrdinal, prefixInstructions)) return false;
        }
        blocks.Add(new GuestBasicBlock(
            activePrefixBlockId,
            prefixInstructions,
            new GuestTerminator("branch", null, firstFlowBlockId, null, null)));

        foreach (SemanticAsyncSegment segment in CollectSynchronousReachable(method, entry.SegmentOrdinal)
            .OrderBy(item => item.Ordinal))
        {
            if (!TryLowerSegment(
                method,
                segment,
                context,
                abi,
                incoming is null,
                blocks,
                diagnostics))
            {
                return false;
            }
        }

        if (blocks.GroupBy(block => block.Id, StringComparer.Ordinal).Any(group => group.Count() != 1))
        {
            Add(diagnostics, method, $"Continuation entry '{entry.FunctionId}' generated duplicate basic-block identities.");
            return false;
        }
        if (!context.ShortCircuitFlow.Rewrite(context, blocks)
            || !CSharpAsyncClosureAllocations.InsertEdges(context, method, blocks))
        {
            return false;
        }
        function = new GuestFunction(
            entry.FunctionId,
            parameters,
            context.Locals,
            incoming is null && method.TaskResultTypeId is not null
                ? callable.ReturnTypeId : abi.VoidType.Id,
            functionEntryBlockId,
            blocks);
        return true;
    }

    private static bool TryLowerSegment(
        SemanticAsyncMethod method,
        SemanticAsyncSegment segment,
        CSharpFunctionLoweringContext context,
        CSharpAsyncAbi abi,
        bool initialEntry,
        List<GuestBasicBlock> blocks,
        List<GuestDiagnostic> diagnostics)
    {
        string activeBlockId = FlowBlockId(method, segment.Ordinal);
        List<GuestInstruction> instructions = new();
        bool activeReachable = true;
        CSharpAsyncControlFlowLowerer structuredFlow = new(
            context,
            segment.Ordinal,
            activeBlockId,
            blocks);
        int diagnosticStart = diagnostics.Count;
        for (int statementOrdinal = 0;
            statementOrdinal < segment.Statements.Count;
            ++statementOrdinal)
        {
            SemanticAsyncStatement statement = segment.Statements[statementOrdinal];
            if (CSharpAsyncControlFlowLowerer.IsStructuredFlow(statement.Operation))
            {
                if (!structuredFlow.Emit(
                    statement.Operation,
                    activeBlockId,
                    instructions,
                    out activeBlockId,
                    out instructions,
                    out activeReachable))
                {
                    return false;
                }
            }
            else
            {
                int instructionStart = instructions.Count;
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
                    return false;
                }
                CSharpGuestDebugTagger.TagFirstEmitted(
                    instructions,
                    instructionStart,
                    statement.Operation,
                    CSharpGuestDebugTagger.OperationId(
                        method.MethodSymbolId,
                        $"async:{segment.Ordinal}",
                        statementOrdinal));
            }
        }
        if (diagnostics.Count != diagnosticStart || !activeReachable || segment.Transfer is null)
        {
            Add(diagnostics, method, $"Continuation CFG segment {segment.Ordinal} did not lower to a reachable transfer.");
            return false;
        }

        SemanticAsyncControlTransfer transfer = segment.Transfer;
        switch (transfer.Kind)
        {
            case SemanticAsyncMethod.GotoTransferKind:
                blocks.Add(new GuestBasicBlock(
                    activeBlockId,
                    instructions,
                    new GuestTerminator(
                        "branch",
                        null,
                        FlowBlockId(method, transfer.PrimaryTarget),
                        null,
                        null)));
                return true;

            case SemanticAsyncMethod.BranchTransferKind:
            {
                int instructionStart = instructions.Count;
                GuestRegister? condition = CSharpOperationLowerer.LowerValue(
                    context,
                    transfer.Condition!,
                    segment.Ordinal,
                    instructions);
                if (condition is null || condition.TypeId != "type:bool")
                {
                    Add(diagnostics, method, $"Continuation CFG segment {segment.Ordinal} has no canonical bool condition.");
                    return false;
                }
                CSharpGuestDebugTagger.TagFirstEmitted(
                    instructions,
                    instructionStart,
                    transfer.Condition!,
                    CSharpGuestDebugTagger.OperationId(
                        method.MethodSymbolId,
                        $"async:{segment.Ordinal}:branch",
                        0));
                blocks.Add(new GuestBasicBlock(
                    activeBlockId,
                    instructions,
                    new GuestTerminator(
                        "branch_if",
                        condition.Id,
                        FlowBlockId(method, transfer.PrimaryTarget),
                        FlowBlockId(method, transfer.SecondaryTarget),
                        null)));
                return true;
            }

            case SemanticAsyncMethod.ReturnTransferKind:
                string? taskReturnValueId = null;
                if (method.TaskResultTypeId is not null)
                {
                    if (abi.TaskResultImportId is null || transfer.Condition is null) return false;
                    GuestRegister? value = CSharpOperationLowerer.LowerValue(
                        context, transfer.Condition, segment.Ordinal, instructions);
                    GuestRegister? task = CSharpTaskResultAbi.LoadProducerToken(
                        context, method, segment.Ordinal, instructions);
                    if (value is null || value.TypeId != CSharpTaskResultAbi.IntTypeId
                        || task is null
                        || CSharpTaskResultAbi.Call(context, CSharpTaskResultAbi.Succeed,
                            task, value, segment.Ordinal, instructions) is null
                        || CSharpTaskResultAbi.Call(context, CSharpTaskResultAbi.Release,
                            task, null, segment.Ordinal, instructions) is null)
                        return false;
                    if (initialEntry)
                    {
                        GuestRegister? returned = CSharpTaskResultAbi.ReturnValue(
                            context, method, segment.Ordinal, instructions);
                        if (returned is null) return false;
                        taskReturnValueId = returned.Id;
                    }
                }
                blocks.Add(new GuestBasicBlock(
                    activeBlockId,
                    instructions,
                    new GuestTerminator(
                        "return",
                        null,
                        null,
                        null,
                        taskReturnValueId,
                        CSharpGuestDebugTagger.Create(
                            segment.Span,
                            CSharpGuestDebugTagger.OperationId(
                                method.MethodSymbolId,
                                $"async:{segment.Ordinal}:return",
                                0),
                            "return"))));
                return true;

            case SemanticAsyncMethod.AwaitTransferKind:
                return TryLowerAwait(
                    method,
                    segment,
                    context,
                    abi,
                    initialEntry,
                    activeBlockId,
                    instructions,
                    blocks,
                    diagnostics);

            default:
                Add(diagnostics, method, $"Continuation CFG segment {segment.Ordinal} has unknown transfer '{transfer.Kind}'.");
                return false;
        }
    }

    private static bool TryLowerAwait(
        SemanticAsyncMethod method,
        SemanticAsyncSegment segment,
        CSharpFunctionLoweringContext context,
        CSharpAsyncAbi abi,
        bool initialEntry,
        string activeBlockId,
        List<GuestInstruction> instructions,
        List<GuestBasicBlock> blocks,
        List<GuestDiagnostic> diagnostics)
    {
        SemanticAsyncAwaitSite? awaitSite = segment.AwaitSite;
        if (awaitSite?.ProducerKind == "task_call")
            return CSharpTaskAwaitLowerer.Lower(method, segment, context, abi,
                initialEntry, activeBlockId, instructions, blocks);
        int awaitInstructionStart = instructions.Count;
        if (awaitSite is null
            || !CSharpAsyncLowerer.EmitProducer(
                context,
                awaitSite,
                abi.DelayImportId,
                abi.ObjectLoadImportId,
                abi.BindCancellationImportId,
                abi.Int32Type,
                abi.Int64Type,
                instructions,
                out GuestRegister? scheduledToken,
                out GuestRegister? cancellationBindingAccepted))
        {
            return false;
        }
        CSharpGuestDebugTagger.TagFirstEmitted(
            instructions,
            awaitInstructionStart,
            awaitSite.Span,
            CSharpGuestDebugTagger.OperationId(
                method.MethodSymbolId,
                $"async:{segment.Ordinal}:await",
                awaitSite.CallbackId),
            "await");

        GuestRegister? stateStoreAccepted = null;
        if (CSharpAsyncClosureState.Frame(context.Document, method, awaitSite) is { } outgoingFrame
            && !CSharpAsyncLowerer.EmitOutgoingState(
                context,
                outgoingFrame,
                awaitSite.CallbackId,
                scheduledToken,
                abi.StateStoreImportId,
                abi.Int32Type,
                instructions,
                out stateStoreAccepted))
        {
            return false;
        }

        GuestRegister? zeroToken = context.CreateTemporary(
            abi.Int64Type.Id,
            segment.Ordinal);
        GuestRegister? scheduleAccepted = context.CreateTemporary(
            abi.Int32Type.Id,
            segment.Ordinal);
        if (scheduledToken is null || zeroToken is null || scheduleAccepted is null)
        {
            return false;
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
            GuestRegister? combined = context.CreateTemporary(
                abi.Int32Type.Id,
                segment.Ordinal);
            if (combined is null)
            {
                return false;
            }
            instructions.Add(new GuestInstruction(
                "binary",
                combined.Id,
                new[] { finalAcceptance.Id, acceptance.Id },
                null,
                "bitwise_and",
                null));
            finalAcceptance = combined;
        }

        string acceptedBlockId = activeBlockId + ":schedule_accepted";
        string rejectedBlockId = activeBlockId + ":schedule_rejected";
        List<GuestInstruction> rejectionInstructions = new();
        if (stateStoreAccepted is not null)
        {
            GuestRegister? cancellationIgnored = context.CreateTemporary(
                abi.Int32Type.Id,
                segment.Ordinal);
            if (cancellationIgnored is null || abi.CancelImportId is null)
            {
                return false;
            }
            rejectionInstructions.Add(new GuestInstruction(
                "call",
                cancellationIgnored.Id,
                new[] { scheduledToken.Id },
                abi.CancelImportId,
                null,
                null));
        }
        blocks.Add(new GuestBasicBlock(
            activeBlockId,
            instructions,
            new GuestTerminator(
                "branch_if",
                finalAcceptance.Id,
                acceptedBlockId,
                rejectedBlockId,
                null)));
        List<GuestInstruction> acceptedInstructions = new();
        string? acceptedReturnId = null;
        if (method.TaskResultTypeId is not null && initialEntry)
        {
            GuestRegister? returned = CSharpTaskResultAbi.ReturnValue(
                context, method, segment.Ordinal, acceptedInstructions);
            if (returned is null) return false;
            acceptedReturnId = returned.Id;
        }
        blocks.Add(new GuestBasicBlock(
            acceptedBlockId,
            acceptedInstructions,
            new GuestTerminator("return", null, null, null, acceptedReturnId)));
        blocks.Add(new GuestBasicBlock(
            rejectedBlockId,
            rejectionInstructions,
            new GuestTerminator("trap", null, null, null, null)));
        return true;
    }

    private static IReadOnlyList<SemanticAsyncSegment> CollectSynchronousReachable(
        SemanticAsyncMethod method,
        int entryOrdinal)
    {
        Dictionary<int, SemanticAsyncSegment> segments = method.Segments
            .ToDictionary(segment => segment.Ordinal);
        HashSet<int> reachable = new();
        Stack<int> pending = new();
        pending.Push(entryOrdinal);
        while (pending.Count > 0)
        {
            int ordinal = pending.Pop();
            if (!reachable.Add(ordinal)
                || !segments.TryGetValue(ordinal, out SemanticAsyncSegment? segment)
                || segment.Transfer is null)
            {
                continue;
            }
            if (segment.Transfer.Kind == SemanticAsyncMethod.GotoTransferKind)
            {
                pending.Push(segment.Transfer.PrimaryTarget);
            }
            else if (segment.Transfer.Kind == SemanticAsyncMethod.BranchTransferKind)
            {
                pending.Push(segment.Transfer.PrimaryTarget);
                pending.Push(segment.Transfer.SecondaryTarget);
            }
            else if (segment.Transfer.Kind == SemanticAsyncMethod.AwaitTransferKind
                && segment.AwaitSite?.ProducerKind == "task_call")
            {
                pending.Push(segment.Transfer.PrimaryTarget);
            }
        }
        return reachable.Select(ordinal => segments[ordinal]).ToArray();
    }

    private static string FlowBlockId(SemanticAsyncMethod method, int ordinal)
    {
        return CSharpGuestIds.AsyncSegmentBlock(method.MethodSymbolId, ordinal);
    }

    private static void Add(
        ICollection<GuestDiagnostic> diagnostics,
        SemanticAsyncMethod method,
        string message)
    {
        diagnostics.Add(new GuestDiagnostic(
            "ASCG1011",
            "error",
            $"{method.MethodSymbolId}: {message}",
            null));
    }

    private sealed record EntryPoint(
        string FunctionId,
        int SegmentOrdinal,
        SemanticAsyncAwaitSite? Incoming);
}
