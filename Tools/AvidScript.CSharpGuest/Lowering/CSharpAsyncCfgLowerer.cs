using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal sealed record CSharpAsyncAbi(
    string? DelayImportId,
    string? CancelResumeDelayImportId,
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
                CSharpGuestIds.AsyncResumeFunction(awaitSite.CallbackId),
                method.Segments.Any(segment => segment.AwaitSite?.CallbackId
                    == awaitSite.CallbackId && CSharpAsyncCancellationLowerer.IsStatusAware(document, method, segment))));
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
        SemanticAsyncControlTransfer? incomingTransfer = incoming is null ? null
            : method.Segments.Single(segment => segment.AwaitSite?.CallbackId
                == incoming.CallbackId).Transfer;
        bool implicitCancellation = incoming is not null && method.Segments.Any(segment =>
            segment.AwaitSite?.CallbackId == incoming.CallbackId
                && CSharpAsyncCancellationLowerer.NeedsImplicitPropagation(document, method, segment));
        bool statusAware = incoming is { ProducerKind: "delay" or "next_tick" }
            && (incomingTransfer?.CancellationTarget is >= 0 || implicitCancellation);
        GuestRegister? directStatus = null;
        if (incoming is not null)
        {
            continuationToken = new GuestRegister(
                CSharpGuestIds.AsyncResumeParameter(incoming.CallbackId, "token"),
                abi.Int64Type.Id);
            parameters.Add(continuationToken);
        }
        if (statusAware)
        {
            directStatus = new GuestRegister(
                CSharpGuestIds.AsyncResumeParameter(incoming!.CallbackId, "status"),
                abi.Int32Type.Id);
            parameters.Add(directStatus);
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
        if (CSharpAsyncCancellationLowerer.HasExceptionStorage(document, method)
            && (context.CreateInternalStorage(CSharpTaskResultAbi.ExceptionSourceSlot(method),
                    CSharpTaskResultAbi.TokenTypeId) is null
                || context.CreateInternalStorage(CSharpTaskResultAbi.ExceptionTypeSlot(method),
                    CSharpTaskResultAbi.IntTypeId) is null)) return false;
        foreach (SemanticAsyncAwaitSite taskAwait in method.Segments
            .Select(segment => segment.AwaitSite)
            .Where(site => site?.ProducerKind is "task_call" or "task_local").Cast<SemanticAsyncAwaitSite>())
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
        if (incoming is null && !CSharpTaskOwnerGuards.Initialize(context, method,
                entry.SegmentOrdinal, prefixInstructions)) return false;
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
                Add(diagnostics, method, $"Continuation '{entry.FunctionId}' could not restore its state frame.");
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
        if (CSharpAsyncCancellationLowerer.HasExceptionStorage(document, method)
            && !CSharpAsyncExceptionLowerer.Initialize(context, method,
                entry.SegmentOrdinal, prefixInstructions)) return false;
        if (incoming?.ProducerKind is "task_call" or "task_local")
        {
            if (!CSharpTaskAwaitLowerer.EmitIncoming(context, method, incoming,
                entry.SegmentOrdinal, prefixInstructions, blocks,
                ref activePrefixBlockId, out List<GuestInstruction>? resumedPrefix))
            {
                Add(diagnostics, method, $"Continuation '{entry.FunctionId}' could not read its Task result.");
                return false;
            }
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
        if (!statusAware && incoming is not null && !CSharpTaskResultAbi.RetainIncomingTaskLocals(
                context, method, incoming, entry.SegmentOrdinal, prefixInstructions)) return false;
        if (!statusAware)
            CSharpAsyncClosureAllocations.Transition(context, method, incomingSegment,
                entry.SegmentOrdinal, prefixInstructions);
        if (incoming?.ResultSymbolId is { } resultSymbol && context.ClosureCells.Has(resultSymbol))
        {
            GuestRegister? value = loadedObject ?? outcome;
            if (value is null || !CSharpOperationLowerer.StoreLocal(context, resultSymbol, value, entry.SegmentOrdinal, prefixInstructions)) return false;
        }
        if (statusAware)
        {
            int cancellationTarget = incomingTransfer!.CancellationTarget ?? -1;
            string cancellationTargetBlock = implicitCancellation
                ? CSharpAsyncCancellationLowerer.ImplicitPropagationBlock(method, incoming!)
                : FlowBlockId(method, cancellationTarget);
            GuestRegister? completed = CSharpTaskResultAbi.Constant(context,
                abi.Int32Type.Id, 1, entry.SegmentOrdinal, prefixInstructions);
            GuestRegister? isCompleted = context.CreateTemporary(abi.Int32Type.Id,
                entry.SegmentOrdinal);
            if (completed is null || isCompleted is null) return false;
            prefixInstructions.Add(new("binary", isCompleted.Id,
                new[] { directStatus!.Id, completed.Id }, null, "equals", null));
            string normalPath = functionEntryBlockId + ":normal_path";
            string cancellationCheck = functionEntryBlockId + ":cancel_check";
            string cancellationPath = functionEntryBlockId + ":cancel_path";
            string invalidStatus = functionEntryBlockId + ":invalid_status";
            blocks.Add(new(activePrefixBlockId, prefixInstructions,
                new("branch_if", isCompleted.Id, normalPath,
                    cancellationCheck, null)));
            List<GuestInstruction> check = new();
            GuestRegister? cancelled = CSharpTaskResultAbi.Constant(context,
                abi.Int32Type.Id, 3, entry.SegmentOrdinal, check);
            GuestRegister? isCancelled = context.CreateTemporary(abi.Int32Type.Id,
                entry.SegmentOrdinal);
            if (cancelled is null || isCancelled is null) return false;
            check.Add(new("binary", isCancelled.Id,
                new[] { directStatus.Id, cancelled.Id }, null, "equals", null));
            blocks.Add(new(cancellationCheck, check,
                new("branch_if", isCancelled.Id, cancellationPath,
                    invalidStatus, null)));
            blocks.Add(new(invalidStatus, Array.Empty<GuestInstruction>(),
                new("trap", null, null, null, null)));
            List<GuestInstruction> normalInstructions = new();
            if (!CSharpTaskResultAbi.RetainIncomingTaskLocals(context, method,
                    incoming!, entry.SegmentOrdinal, normalInstructions)) return false;
            CSharpAsyncClosureAllocations.Transition(context, method,
                incomingSegment, entry.SegmentOrdinal, normalInstructions);
            blocks.Add(new(normalPath, normalInstructions,
                new("branch", null, firstFlowBlockId, null, null)));
            List<GuestInstruction> cancellationInstructions = new();
            if (!CSharpTaskResultAbi.RetainIncomingTaskLocals(context, method,
                    incoming!, implicitCancellation ? entry.SegmentOrdinal : cancellationTarget, cancellationInstructions)) return false;
            CSharpAsyncClosureAllocations.Transition(context, method,
                incomingSegment, cancellationTarget, cancellationInstructions);
            if (CSharpTaskResultAbi.SupportsCancellation(document))
            {
                if (!CSharpAsyncCancellationLowerer.EmitDirect(context, method, incoming!,
                        entry.SegmentOrdinal, cancellationPath,
                        cancellationTargetBlock, cancellationInstructions, blocks)) return false;
                if (implicitCancellation)
                {
                    string propagation = cancellationTargetBlock + ":propagate_exception";
                    blocks.Add(new(cancellationTargetBlock, Array.Empty<GuestInstruction>(),
                        new("branch", null, propagation, null, null)));
                    if (!TryLowerExceptionPropagation(method, method.Segments[incomingSegment!.Value], context,
                            false, propagation, new List<GuestInstruction>(), blocks)) return false;
                }
            }
            else
                blocks.Add(new(cancellationPath, cancellationInstructions,
                    new("branch", null, FlowBlockId(method, cancellationTarget), null, null)));
        }
        else
        {
            blocks.Add(new GuestBasicBlock(activePrefixBlockId, prefixInstructions,
                new GuestTerminator("branch", null, firstFlowBlockId, null, null)));
        }

        List<int> entryTargets = new() { entry.SegmentOrdinal };
        if (incoming is not null && method.ExceptionPlan is not null)
        {
            if (incomingTransfer?.SecondaryTarget is >= 0)
                entryTargets.Add(incomingTransfer.SecondaryTarget);
            if (incomingTransfer?.CancellationTarget is int cancellationTarget)
                entryTargets.Add(cancellationTarget);
        }
        foreach (SemanticAsyncSegment segment in entryTargets
            .SelectMany(target => CollectSynchronousReachable(method, target))
            .DistinctBy(item => item.Ordinal)
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
                Add(diagnostics, method, $"Continuation '{entry.FunctionId}' could not lower segment {segment.Ordinal}.");
                return false;
            }
        }

        if (blocks.GroupBy(block => block.Id, StringComparer.Ordinal).Any(group => group.Count() != 1))
        {
            Add(diagnostics, method, $"Continuation entry '{entry.FunctionId}' generated duplicate basic-block identities.");
            return false;
        }
        if (!context.ShortCircuitFlow.Rewrite(context, blocks)
            || !CSharpAsyncClosureAllocations.InsertEdges(context, method, blocks)
            || !CSharpTaskLocalLifetimes.InsertEdges(context, method, blocks, incomingSegment, functionEntryBlockId))
        {
            Add(diagnostics, method, $"Continuation '{entry.FunctionId}' could not finalize its control-flow blocks.");
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
            int taskInstructionStart = instructions.Count;
            if (!CSharpTaskLocalLifetimes.LowerWrite(context, method, segment, statementOrdinal, instructions, out bool taskWrite)) return false;
            if (taskWrite)
            {
                CSharpGuestDebugTagger.TagFirstEmitted(instructions, taskInstructionStart, statement.Operation,
                    CSharpGuestDebugTagger.OperationId(method.MethodSymbolId, $"async:{segment.Ordinal}", statementOrdinal));
                continue;
            }
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
                if (statement.TargetSymbolId is { } aliasId
                    && method.TaskLocalSymbolIds?.Contains(aliasId, StringComparer.Ordinal) == true
                    && statement.Operation is { Kind: "local_reference", SymbolId: { } sourceId }
                    && method.TaskLocalSymbolIds.Contains(sourceId, StringComparer.Ordinal))
                {
                    GuestRegister? token = context.CreateTemporary(CSharpTaskResultAbi.TokenTypeId,
                        segment.Ordinal);
                    if (value is null || token is null) return false;
                    instructions.Add(new GuestInstruction("convert", token.Id,
                        new[] { value.Id }, null, null, null));
                    // Retain returns an acceptance flag; the local still stores
                    // the original task token held by the source local.
                    if (CSharpTaskResultAbi.Call(context, CSharpTaskResultAbi.Retain,
                            token, null, segment.Ordinal, instructions) is null) return false;
                }
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
            case SemanticAsyncMethod.RaiseExceptionTransferKind:
                return CSharpAsyncThrowLowerer.Emit(context, method, segment, activeBlockId, instructions, blocks);
            case SemanticAsyncMethod.EndCatchTransferKind:
                blocks.Add(new(activeBlockId, instructions,
                    new("branch", null, activeBlockId + ":end_catch", null, null)));
                activeBlockId += ":end_catch";
                instructions = new List<GuestInstruction>();
                if (!CSharpAsyncExceptionLowerer.ReleaseIfHeld(context, method,
                        segment.Ordinal, blocks, ref activeBlockId, ref instructions)) return false;
                blocks.Add(new(activeBlockId, instructions,
                    new("branch", null, FlowBlockId(method, transfer.PrimaryTarget), null, null)));
                return true;

            case SemanticAsyncMethod.RethrowTransferKind:
                blocks.Add(new(activeBlockId, instructions,
                    new("branch", null, activeBlockId + ":rethrow", null, null)));
                blocks.Add(new(activeBlockId + ":rethrow", Array.Empty<GuestInstruction>(),
                    new("branch", null, FlowBlockId(method, transfer.PrimaryTarget), null, null)));
                return true;

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
                if (!CSharpAsyncExceptionLowerer.ReleaseIfHeld(context, method,
                        segment.Ordinal, blocks, ref activeBlockId,
                        ref instructions)) return false;
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
                            task, value, segment.Ordinal, instructions) is null)
                        return false;
                    if (initialEntry)
                    {
                        if (CSharpTaskResultAbi.Call(context, CSharpTaskResultAbi.Release,
                                task, null, segment.Ordinal, instructions) is null) return false;
                        GuestRegister? returned = CSharpTaskResultAbi.ReturnValue(
                            context, method, segment.Ordinal, instructions);
                        if (returned is null) return false;
                        taskReturnValueId = returned.Id;
                    }
                }
                if (!CSharpTaskResultAbi.ReleaseTaskLocal(context, method,
                        segment.Ordinal, instructions)) return false;
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

            case SemanticAsyncMethod.ThrowTransferKind:
                return TryLowerThrow(method, segment, context, initialEntry,
                    activeBlockId, instructions, blocks, diagnostics);

            case SemanticAsyncMethod.CatchMatchTransferKind:
                return CSharpAsyncExceptionLowerer.EmitCatchMatch(context, method,
                    transfer, segment.Ordinal, activeBlockId, instructions, blocks);

            case SemanticAsyncMethod.PropagateFaultTransferKind:
            case SemanticAsyncMethod.PropagateCancellationTransferKind:
                return TryLowerExceptionPropagation(method, segment, context,
                    initialEntry, activeBlockId, instructions, blocks);

            case SemanticAsyncMethod.PropagateExceptionTransferKind:
                blocks.Add(new(activeBlockId, instructions,
                    new("branch", null, activeBlockId + ":propagate_exception", null, null)));
                return TryLowerExceptionPropagation(method, segment, context,
                    initialEntry, activeBlockId + ":propagate_exception", new List<GuestInstruction>(), blocks);

            default:
                Add(diagnostics, method, $"Continuation CFG segment {segment.Ordinal} has unknown transfer '{transfer.Kind}'.");
                return false;
        }
    }

    private static bool TryLowerThrow(
        SemanticAsyncMethod method,
        SemanticAsyncSegment segment,
        CSharpFunctionLoweringContext context,
        bool initialEntry,
        string activeBlockId,
        List<GuestInstruction> instructions,
        List<GuestBasicBlock> blocks,
        List<GuestDiagnostic> diagnostics)
    {
        SemanticAsyncThrowSite? site = method.ErrorPlan?.Throws.SingleOrDefault(item =>
            item.SegmentOrdinal == segment.Ordinal);
        if (site is null || method.TaskResultTypeId != CSharpTaskResultAbi.IntTypeId
            || context.Document.SchemaVersion is not
                (SemanticContract.AsyncLanguageErrorSchemaVersion
                    or SemanticContract.AsyncExceptionFlowSchemaVersion
                    or SemanticContract.DirectAwaitCleanupSchemaVersion
                    or SemanticContract.AsyncCancellationFlowSchemaVersion
                    or SemanticContract.TaskLocalLifetimeSchemaVersion
                    or SemanticContract.AsyncThrowRoutingSchemaVersion))
        {
            Add(diagnostics, method, "Async throw has no validated Task<int> language-error site.");
            return false;
        }
        if (!CSharpAsyncExceptionLowerer.ReleaseIfHeld(context, method,
                segment.Ordinal, blocks, ref activeBlockId,
                ref instructions)) return false;
        CSharpLanguageErrorTokenCatalog catalog =
            CSharpAsyncLanguageErrorCatalog.Build(context.Document);
        int typeToken = catalog.Types.Single(item => item.TypeId == site.ExceptionTypeId).Token;
        int sourceToken = catalog.Sources.Single(item => item.SourceId == method.ErrorPlan!.SourceId
            && item.Span == site.Span).Token;
        GuestRegister? type = CSharpTaskResultAbi.Constant(context,
            CSharpTaskResultAbi.IntTypeId, typeToken, segment.Ordinal, instructions);
        GuestRegister? source = CSharpTaskResultAbi.Constant(context,
            CSharpTaskResultAbi.IntTypeId, sourceToken, segment.Ordinal, instructions);
        GuestRegister? root = context.CreateTemporary("type:language_error_root", segment.Ordinal);
        GuestRegister? task = CSharpTaskResultAbi.LoadProducerToken(context,
            method, segment.Ordinal, instructions);
        GuestRegister? accepted = context.CreateTemporary(CSharpTaskResultAbi.IntTypeId,
            segment.Ordinal);
        if (type is null || source is null || root is null || task is null
            || accepted is null) return false;
        int firstInstruction = instructions.Count;
        instructions.Add(new("managed_new", root.Id, Array.Empty<string>(), null, null, null));
        instructions.Add(new("managed_set", null, new[] { root.Id, type.Id },
            "field:code", null, null));
        instructions.Add(new("call", accepted.Id,
            new[] { task.Id, type.Id, source.Id, root.Id },
            CSharpTaskResultAbi.FaultLanguageErrorImportId, null, null));
        CSharpGuestDebugTagger.TagFirstEmitted(instructions, firstInstruction,
            site.Span, CSharpGuestDebugTagger.OperationId(
                method.MethodSymbolId, $"async:{segment.Ordinal}:throw", 0), "statement");

        string completed = activeBlockId + ":task_fault_accepted";
        string rejected = activeBlockId + ":task_fault_rejected";
        blocks.Add(new(activeBlockId, instructions,
            new("branch_if", accepted.Id, completed, rejected, null)));
        List<GuestInstruction> release = new();
        if (!CSharpTaskResultAbi.ReleaseTaskLocal(context, method,
                segment.Ordinal, release)) return false;
        string? returnValue = null;
        if (initialEntry)
        {
            if (CSharpTaskResultAbi.Call(context, CSharpTaskResultAbi.Release,
                    task, null, segment.Ordinal, release) is null) return false;
            returnValue = CSharpTaskResultAbi.ReturnValue(context, method,
                segment.Ordinal, release)?.Id;
            if (returnValue is null) return false;
        }
        blocks.Add(new(completed, release,
            new("return", null, null, null, returnValue)));
        blocks.Add(new(rejected, Array.Empty<GuestInstruction>(),
            new("trap", null, null, null, null)));
        return true;
    }

    private static bool TryLowerExceptionPropagation(
        SemanticAsyncMethod method, SemanticAsyncSegment segment,
        CSharpFunctionLoweringContext context, bool initialEntry,
        string activeBlockId, List<GuestInstruction> instructions,
        List<GuestBasicBlock> blocks)
    {
        if (!CSharpAsyncCancellationLowerer.HasExceptionStorage(context.Document, method) || method.TaskResultTypeId is null)
            return false;
        GuestRegister? owner = CSharpAsyncExceptionLowerer.LoadOwner(context,
            method, segment.Ordinal, instructions);
        bool directCancellation = context.Document.SchemaVersion
                == SemanticContract.DirectAwaitCleanupSchemaVersion
            && segment.Transfer?.Kind
                == SemanticAsyncMethod.PropagateCancellationTransferKind;
        bool hasProtectedTaskAwait = method.Segments.Any(candidate =>
            candidate.AwaitSite?.ProducerKind is "task_call" or "task_local"
            && candidate.Transfer?.SecondaryTarget is >= 0);
        if (directCancellation && !hasProtectedTaskAwait)
        {
            if (initialEntry || !CSharpTaskResultAbi.ReleaseTaskLocal(context,
                    method, segment.Ordinal, instructions)) return false;
            blocks.Add(new(activeBlockId, instructions,
                new("return", null, null, null, null)));
            return true;
        }
        if (directCancellation)
        {
            if (initialEntry || owner is null) return false;
            GuestRegister? zeroOwner = CSharpTaskResultAbi.Constant(context,
                CSharpTaskResultAbi.TokenTypeId, 0, segment.Ordinal, instructions);
            GuestRegister? hasOwner = context.CreateTemporary(
                CSharpTaskResultAbi.IntTypeId, segment.Ordinal);
            if (zeroOwner is null || hasOwner is null) return false;
            instructions.Add(new("binary", hasOwner.Id,
                new[] { owner.Id, zeroOwner.Id }, null, "not_equals", null));
            string taskCancellation = activeBlockId + ":task_cancellation";
            string directCompletion = activeBlockId + ":direct_cancellation";
            blocks.Add(new(activeBlockId, instructions,
                new("branch_if", hasOwner.Id, taskCancellation,
                    directCompletion, null)));
            List<GuestInstruction> directInstructions = new();
            if (!CSharpTaskResultAbi.ReleaseTaskLocal(context, method,
                    segment.Ordinal, directInstructions)) return false;
            blocks.Add(new(directCompletion, directInstructions,
                new("return", null, null, null, null)));
            activeBlockId = taskCancellation;
            instructions = new List<GuestInstruction>();
        }
        GuestRegister? accepted = owner is null ? null
            : CSharpTaskResultAbi.PropagateFailure(context, method, owner,
                segment.Ordinal, instructions);
        if (accepted is null) return false;
        string propagated = activeBlockId + ":propagated";
        string rejected = activeBlockId + ":propagate_rejected";
        blocks.Add(new(activeBlockId, instructions,
            new("branch_if", accepted.Id, propagated, rejected, null)));
        blocks.Add(new(rejected, Array.Empty<GuestInstruction>(),
            new("trap", null, null, null, null)));
        activeBlockId = propagated;
        instructions = new List<GuestInstruction>();
        if (!CSharpAsyncExceptionLowerer.ReleaseIfHeld(context, method,
                segment.Ordinal, blocks, ref activeBlockId,
                ref instructions)
            || !CSharpTaskResultAbi.ReleaseTaskLocal(context, method,
                segment.Ordinal, instructions)) return false;
        string? returnValue = null;
        if (initialEntry)
        {
            GuestRegister? producer = CSharpTaskResultAbi.LoadProducerToken(
                context, method, segment.Ordinal, instructions);
            if (producer is null || CSharpTaskResultAbi.Call(context,
                    CSharpTaskResultAbi.Release, producer, null,
                    segment.Ordinal, instructions) is null) return false;
            returnValue = CSharpTaskResultAbi.ReturnValue(context, method,
                segment.Ordinal, instructions)?.Id;
            if (returnValue is null) return false;
        }
        blocks.Add(new(activeBlockId, instructions,
            new("return", null, null, null, returnValue)));
        return true;
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
        if (awaitSite?.ProducerKind is "task_call" or "task_local")
            return CSharpTaskAwaitLowerer.Lower(method, segment, context, abi,
                initialEntry, activeBlockId, instructions, blocks);
        int awaitInstructionStart = instructions.Count;
        if (awaitSite is null
            || !CSharpAsyncLowerer.EmitProducer(
                context,
                awaitSite,
                abi.DelayImportId,
                abi.CancelResumeDelayImportId,
                CSharpAsyncCancellationLowerer.IsStatusAware(context.Document, method, segment),
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

        if (scheduledToken is null) return false;
        GuestRegister? producerBindingAccepted = method.TaskResultTypeId is null
            ? null
            : CSharpTaskResultAbi.BindProducer(context, method, scheduledToken,
                segment.Ordinal, instructions);
        if (method.TaskResultTypeId is not null && producerBindingAccepted is null)
            return false;

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
            producerBindingAccepted,
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
        if (!CSharpTaskResultAbi.ReleaseTaskLocal(context, method,
                segment.Ordinal, rejectionInstructions)) return false;
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
        if (!CSharpTaskResultAbi.TransferTaskLocalToContinuation(context,
                method, scheduledToken, segment.Ordinal, acceptedInstructions)) return false;
        if (method.TaskResultTypeId is not null && initialEntry)
        {
            GuestRegister? producer = CSharpTaskResultAbi.LoadProducerToken(
                context, method, segment.Ordinal, acceptedInstructions);
            if (producer is null || CSharpTaskResultAbi.Call(context,
                    CSharpTaskResultAbi.Release, producer, null,
                    segment.Ordinal, acceptedInstructions) is null) return false;
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
            if (segment.Transfer.Kind is SemanticAsyncMethod.GotoTransferKind
                or SemanticAsyncMethod.EndCatchTransferKind or SemanticAsyncMethod.RethrowTransferKind
                or SemanticAsyncMethod.RaiseExceptionTransferKind)
            {
                pending.Push(segment.Transfer.PrimaryTarget);
            }
            else if (segment.Transfer.Kind == SemanticAsyncMethod.BranchTransferKind)
            {
                pending.Push(segment.Transfer.PrimaryTarget);
                pending.Push(segment.Transfer.SecondaryTarget);
            }
            else if (segment.Transfer.Kind == SemanticAsyncMethod.CatchMatchTransferKind)
            {
                pending.Push(segment.Transfer.PrimaryTarget);
                pending.Push(segment.Transfer.SecondaryTarget);
            }
            else if (segment.Transfer.Kind == SemanticAsyncMethod.AwaitTransferKind
                && segment.AwaitSite?.ProducerKind is ("task_call" or "task_local"))
            {
                pending.Push(segment.Transfer.PrimaryTarget);
                if (segment.Transfer.SecondaryTarget >= 0)
                    pending.Push(segment.Transfer.SecondaryTarget);
                if (segment.Transfer.CancellationTarget is int cancellationTarget)
                    pending.Push(cancellationTarget);
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
