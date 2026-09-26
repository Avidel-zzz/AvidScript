using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal static class CSharpTaskAwaitLowerer
{
    public static bool Lower(SemanticAsyncMethod method, SemanticAsyncSegment segment,
        CSharpFunctionLoweringContext context, CSharpAsyncAbi abi, bool initialEntry,
        string blockId, List<GuestInstruction> instructions, List<GuestBasicBlock> blocks)
    {
        SemanticAsyncAwaitSite? site = segment.AwaitSite;
        bool taskLocal = site?.ProducerKind == "task_local";
        SemanticCallable? target = context.Document.Callables.SingleOrDefault(callable =>
            callable.MethodSymbolId == site?.TaskCallableId);
        if (site is null || target is null || abi.TaskResultImportId is null
            || segment.Transfer?.Kind != SemanticAsyncMethod.AwaitTransferKind
            || (taskLocal ? site.Arguments.Count != 1
                : site.Arguments.Count != target.Parameters.Count)
            || !context.TryGetStorage(CSharpTaskResultAbi.AwaitSlot(site), out GuestRegister storage))
        {
            context.Add("ASCG1010", "Task<int> await has no validated target or state storage.");
            return false;
        }

        GuestRegister? token;
        if (taskLocal)
        {
            token = CSharpTaskResultAbi.LoadTaskLocalToken(
                context, site!.TaskLocalSymbolId!, segment.Ordinal, instructions);
        }
        else
        {
            GuestRegister? taskValue = context.CreateTemporary(target.ReturnTypeId, segment.Ordinal);
            token = context.CreateTemporary(CSharpTaskResultAbi.TokenTypeId, segment.Ordinal);
            if (taskValue is null || token is null) return false;
            List<string> arguments = new(site.Arguments.Count);
            SemanticCallableParameter[] parameters = target.Parameters.OrderBy(parameter => parameter.Ordinal).ToArray();
            for (int index = 0; index < site.Arguments.Count; ++index)
            {
                GuestRegister? argument = CSharpOperationLowerer.LowerValue(
                    context, site.Arguments[index], segment.Ordinal, instructions);
                if (argument is null || argument.TypeId != parameters[index].TypeId) return false;
                arguments.Add(argument.Id);
            }
            instructions.Add(new("call", taskValue.Id, arguments,
                CSharpGuestIds.Function(target.MethodSymbolId), null, null));
            instructions.Add(new("convert", token.Id, new[] { taskValue.Id }, null, null, null));
        }
        if (token is null) return false;
        instructions.Add(new("local_store", null, new[] { token.Id }, storage.Id, null, null));
        GuestRegister? callback = CSharpTaskResultAbi.Constant(context,
            CSharpTaskResultAbi.IntTypeId, site.CallbackId, segment.Ordinal, instructions);
        GuestRegister? scheduled = callback is null ? null : CSharpTaskResultAbi.Call(context,
            CSharpTaskResultAbi.Await, token, callback, segment.Ordinal, instructions);
        GuestRegister? zero = CSharpTaskResultAbi.Constant(context,
            CSharpTaskResultAbi.TokenTypeId, 0, segment.Ordinal, instructions);
        GuestRegister? ready = context.CreateTemporary(CSharpTaskResultAbi.IntTypeId, segment.Ordinal);
        if (scheduled is null || zero is null || ready is null) return false;
        instructions.Add(new("binary", ready.Id, new[] { scheduled.Id, zero.Id },
            null, "equals", null));
        string readyBlock = blockId + ":task_ready";
        string pendingBlock = blockId + ":task_pending";
        blocks.Add(new(blockId, instructions,
            new("branch_if", ready.Id, readyBlock, pendingBlock, null)));

        List<GuestInstruction> readyInstructions = new();
        if (!EmitRead(context, token, segment.Ordinal, readyInstructions,
            out GuestRegister? value, out GuestRegister? succeeded,
            out GuestRegister? state)) return false;
        string valueBlock = blockId + ":task_value";
        string failedBlock = blockId + ":task_failed";
        blocks.Add(new(readyBlock, readyInstructions,
            new("branch_if", succeeded!.Id, valueBlock, failedBlock, null)));
        List<GuestInstruction> failedInstructions = new();
        if (method.ExceptionPlan is not null)
        {
            if (!CSharpAsyncExceptionLowerer.EmitFailure(context, method,
                    segment.Transfer!, token, state!, segment.Ordinal,
                    failedBlock, failedInstructions, blocks,
                    releaseDirectToken: !taskLocal)) return false;
        }
        else if (method.TaskResultTypeId is null
            && ReportsUnhandledLanguageError(context.Document))
        {
            if (!EmitUnhandledFailure(context, method, token, state!, segment.Ordinal,
                    failedBlock, failedInstructions, blocks,
                    releaseDirectToken: !taskLocal, releaseTaskLocals: true)) return false;
        }
        else
        {
            if (method.TaskResultTypeId is not null
                && CSharpTaskResultAbi.PropagateFailure(context, method, token,
                    segment.Ordinal, failedInstructions) is null) return false;
            if ((!taskLocal && CSharpTaskResultAbi.Call(context, CSharpTaskResultAbi.Release,
                    token, null, segment.Ordinal, failedInstructions) is null)
                || !CSharpTaskResultAbi.ReleaseTaskLocal(context, method,
                    segment.Ordinal, failedInstructions)) return false;
            string? failedReturnId = null;
            if (method.TaskResultTypeId is not null && initialEntry)
            {
                GuestRegister? producer = CSharpTaskResultAbi.LoadProducerToken(
                    context, method, segment.Ordinal, failedInstructions);
                if (producer is null || CSharpTaskResultAbi.Call(context,
                        CSharpTaskResultAbi.Release, producer, null,
                        segment.Ordinal, failedInstructions) is null) return false;
                failedReturnId = CSharpTaskResultAbi.ReturnValue(context, method,
                    segment.Ordinal, failedInstructions)?.Id;
                if (failedReturnId is null) return false;
            }
            blocks.Add(new(failedBlock, failedInstructions,
                new(method.TaskResultTypeId is null ? "trap" : "return",
                    null, null, null, failedReturnId)));
        }
        List<GuestInstruction> valueInstructions = new();
        if (!StoreResult(context, site, value!, segment.Ordinal, valueInstructions)) return false;
        if (!taskLocal && CSharpTaskResultAbi.Call(context, CSharpTaskResultAbi.Release,
                token, null, segment.Ordinal, valueInstructions) is null) return false;
        blocks.Add(new(valueBlock, valueInstructions,
            new("branch", null,
                CSharpGuestIds.AsyncSegmentBlock(method.MethodSymbolId,
                    segment.Transfer.PrimaryTarget), null, null)));

        List<GuestInstruction> pendingInstructions = new();
        SemanticAsyncStateFrame? frame = CSharpAsyncClosureState.Frame(context.Document, method, site);
        if (frame is null || !CSharpAsyncLowerer.EmitOutgoingState(context, frame,
                site.CallbackId, scheduled, abi.StateStoreImportId, abi.Int32Type,
                pendingInstructions, out GuestRegister? stateAccepted)
            || stateAccepted is null) return false;
        GuestRegister finalAcceptance = stateAccepted;
        if (method.TaskResultTypeId is not null)
        {
            GuestRegister? bound = CSharpTaskResultAbi.BindProducer(
                context, method, scheduled, segment.Ordinal, pendingInstructions);
            GuestRegister? combined = context.CreateTemporary(
                CSharpTaskResultAbi.IntTypeId, segment.Ordinal);
            if (bound is null || combined is null) return false;
            pendingInstructions.Add(new("binary", combined.Id,
                new[] { stateAccepted.Id, bound.Id }, null, "bitwise_and", null));
            finalAcceptance = combined;
        }
        string acceptedBlock = blockId + ":task_pending_accepted";
        string rejectedBlock = blockId + ":task_pending_rejected";
        blocks.Add(new(pendingBlock, pendingInstructions,
            new("branch_if", finalAcceptance.Id, acceptedBlock, rejectedBlock, null)));
        List<GuestInstruction> acceptedInstructions = new();
        if (!CSharpTaskResultAbi.TransferTaskLocalToContinuation(context,
                method, scheduled, segment.Ordinal, acceptedInstructions)
            || !taskLocal && CSharpTaskResultAbi.Call(context, CSharpTaskResultAbi.Release,
                token, null, segment.Ordinal, acceptedInstructions) is null) return false;
        if (method.TaskResultTypeId is not null && initialEntry)
        {
            GuestRegister? producer = CSharpTaskResultAbi.LoadProducerToken(
                context, method, segment.Ordinal, acceptedInstructions);
            if (producer is null || CSharpTaskResultAbi.Call(context,
                    CSharpTaskResultAbi.Release, producer, null,
                    segment.Ordinal, acceptedInstructions) is null) return false;
        }
        string? returnId = null;
        if (method.TaskResultTypeId is not null && initialEntry)
        {
            GuestRegister? returned = CSharpTaskResultAbi.ReturnValue(context,
                method, segment.Ordinal, acceptedInstructions);
            if (returned is null) return false;
            returnId = returned.Id;
        }
        blocks.Add(new(acceptedBlock, acceptedInstructions,
            new("return", null, null, null, returnId)));
        List<GuestInstruction> rejectedInstructions = new();
        if (abi.CancelImportId is not null)
        {
            GuestRegister? ignored = context.CreateTemporary(CSharpTaskResultAbi.IntTypeId, segment.Ordinal);
            if (ignored is null) return false;
            rejectedInstructions.Add(new("call", ignored.Id,
                new[] { scheduled.Id }, abi.CancelImportId, null, null));
        }
        if ((!taskLocal && CSharpTaskResultAbi.Call(context, CSharpTaskResultAbi.Release,
                token, null, segment.Ordinal, rejectedInstructions) is null)
            || !CSharpTaskResultAbi.ReleaseTaskLocal(context, method,
                segment.Ordinal, rejectedInstructions)) return false;
        blocks.Add(new(rejectedBlock, rejectedInstructions,
            new("trap", null, null, null, null)));
        return true;
    }

    public static bool EmitIncoming(CSharpFunctionLoweringContext context,
        SemanticAsyncMethod method, SemanticAsyncAwaitSite site,
        int block, List<GuestInstruction> instructions,
        List<GuestBasicBlock> blocks, ref string activeBlockId,
        out List<GuestInstruction>? nextInstructions)
    {
        nextInstructions = null;
        if (!context.TryGetStorage(CSharpTaskResultAbi.AwaitSlot(site), out GuestRegister storage))
            return false;
        GuestRegister? token = context.CreateTemporary(CSharpTaskResultAbi.TokenTypeId, block);
        if (token is null) return false;
        instructions.Add(new("local_load", token.Id, Array.Empty<string>(), storage.Id, null, null));
        if (!EmitRead(context, token, block, instructions,
            out GuestRegister? value, out GuestRegister? succeeded,
            out GuestRegister? state)) return false;
        string accepted = activeBlockId + ":task_read_accepted";
        string rejected = activeBlockId + ":task_read_rejected";
        blocks.Add(new(activeBlockId, instructions,
            new("branch_if", succeeded!.Id, accepted, rejected, null)));
        List<GuestInstruction> rejectedInstructions = new();
        SemanticAsyncSegment? incomingSegment = method.Segments.SingleOrDefault(segment =>
            segment.AwaitSite?.CallbackId == site.CallbackId);
        if (method.ExceptionPlan is not null)
        {
            if (incomingSegment?.Transfer is not { } transfer
                || !CSharpAsyncExceptionLowerer.EmitFailure(context, method,
                    transfer, token, state!, block, rejected,
                    rejectedInstructions, blocks, releaseDirectToken: false)) return false;
        }
        else if (method.TaskResultTypeId is null
            && ReportsUnhandledLanguageError(context.Document))
        {
            if (!EmitUnhandledFailure(context, method, token, state!, block,
                    rejected, rejectedInstructions, blocks,
                    releaseDirectToken: false, releaseTaskLocals: false)) return false;
        }
        else
        {
            if (method.TaskResultTypeId is not null
                && CSharpTaskResultAbi.PropagateFailure(context, method, token,
                    block, rejectedInstructions) is null) return false;
            blocks.Add(new(rejected, rejectedInstructions,
                new(method.TaskResultTypeId is null ? "trap" : "return",
                    null, null, null, null)));
        }
        nextInstructions = new();
        if (!StoreResult(context, site, value!, block, nextInstructions)) return false;
        activeBlockId = accepted;
        return true;
    }

    private static bool EmitUnhandledFailure(CSharpFunctionLoweringContext context,
        SemanticAsyncMethod method, GuestRegister token, GuestRegister state,
        int block, string failedBlockId, List<GuestInstruction> instructions,
        List<GuestBasicBlock> blocks, bool releaseDirectToken,
        bool releaseTaskLocals)
    {
        GuestRegister? faultedState = CSharpTaskResultAbi.Constant(context,
            CSharpTaskResultAbi.TokenTypeId, 2, block, instructions);
        GuestRegister? faulted = context.CreateTemporary(CSharpTaskResultAbi.IntTypeId, block);
        if (faultedState is null || faulted is null) return false;
        instructions.Add(new("binary", faulted.Id,
            new[] { state.Id, faultedState.Id }, null, "equals", null));
        GuestRegister hasLanguageError = faulted;
        if (CSharpTaskResultAbi.SupportsCancellation(context.Document))
        {
            GuestRegister? cancelledState = CSharpTaskResultAbi.Constant(context,
                CSharpTaskResultAbi.TokenTypeId, 3, block, instructions);
            GuestRegister? cancelled = context.CreateTemporary(CSharpTaskResultAbi.IntTypeId, block);
            GuestRegister? terminalError = context.CreateTemporary(CSharpTaskResultAbi.IntTypeId, block);
            if (cancelledState is null || cancelled is null || terminalError is null) return false;
            instructions.Add(new("binary", cancelled.Id,
                new[] { state.Id, cancelledState.Id }, null, "equals", null));
            instructions.Add(new("binary", terminalError.Id,
                new[] { faulted.Id, cancelled.Id }, null, "bitwise_or", null));
            hasLanguageError = terminalError;
        }
        string languageErrorBlock = failedBlockId + ":language_error";
        string otherFailureBlock = failedBlockId + ":other_failure";
        blocks.Add(new(failedBlockId, instructions,
            new("branch_if", hasLanguageError.Id, languageErrorBlock, otherFailureBlock, null)));

        List<GuestInstruction> report = new();
        CSharpTaskLanguageError? error = CSharpTaskResultAbi.ReadLanguageError(
            context, token, block, report);
        GuestRegister? reported = context.CreateTemporary(CSharpTaskResultAbi.IntTypeId, block);
        if (error is null || reported is null) return false;
        if (!ReleaseOwners(report)) return false;
        report.Add(new("call", reported.Id,
            new[] { error.TypeToken.Id, error.SourceToken.Id, error.Root.Id },
            CSharpTaskResultAbi.LanguageErrorReportImportId, null, null));
        blocks.Add(new(languageErrorBlock, report,
            new("trap", null, null, null, null)));

        List<GuestInstruction> other = new();
        if (!ReleaseOwners(other)) return false;
        blocks.Add(new(otherFailureBlock, other,
            new("trap", null, null, null, null)));
        return true;

        bool ReleaseOwners(List<GuestInstruction> output) =>
            (!releaseDirectToken || CSharpTaskResultAbi.Call(context,
                CSharpTaskResultAbi.Release, token, null, block, output) is not null)
            && (!releaseTaskLocals || CSharpTaskResultAbi.ReleaseTaskLocal(
                context, method, block, output));
    }

    private static bool ReportsUnhandledLanguageError(SemanticDocument document) =>
        (document.SchemaVersion == SemanticContract.AsyncLanguageErrorSchemaVersion
            && document.SemanticVersion == SemanticContract.AsyncLanguageErrorSemanticVersion)
        || (document.SchemaVersion == SemanticContract.AsyncExceptionFlowSchemaVersion
            && document.SemanticVersion == SemanticContract.AsyncExceptionFlowSemanticVersion)
        || (document.SchemaVersion == SemanticContract.DirectAwaitCleanupSchemaVersion
            && document.SemanticVersion == SemanticContract.DirectAwaitCleanupSemanticVersion)
        || CSharpTaskResultAbi.SupportsCancellation(document);

    private static bool StoreResult(CSharpFunctionLoweringContext context,
        SemanticAsyncAwaitSite site, GuestRegister value, int block,
        List<GuestInstruction> instructions)
    {
        if (site.ResultSymbolId is null) return true;
        if (site.ResultStorageKind == "static_field")
        {
            if (!context.TryGetGlobal(site.ResultSymbolId, out string globalId)) return false;
            instructions.Add(new("global_store", null, new[] { value.Id }, globalId, null, null));
            return true;
        }
        return (site.ResultStorageKind is null or "existing_local")
            && CSharpOperationLowerer.StoreLocal(context, site.ResultSymbolId,
                value, block, instructions);
    }

    private static bool EmitRead(CSharpFunctionLoweringContext context,
        GuestRegister token, int block, List<GuestInstruction> instructions,
        out GuestRegister? value, out GuestRegister? succeeded,
        out GuestRegister? state)
    {
        value = null;
        succeeded = null;
        state = null;
        GuestRegister? packed = CSharpTaskResultAbi.Call(context,
            CSharpTaskResultAbi.Read, token, null, block, instructions);
        GuestRegister? shift = CSharpTaskResultAbi.Constant(context,
            CSharpTaskResultAbi.TokenTypeId, 32, block, instructions);
        state = context.CreateTemporary(CSharpTaskResultAbi.TokenTypeId, block);
        GuestRegister? successState = CSharpTaskResultAbi.Constant(context,
            CSharpTaskResultAbi.TokenTypeId, 1, block, instructions);
        succeeded = context.CreateTemporary(CSharpTaskResultAbi.IntTypeId, block);
        value = context.CreateTemporary(CSharpTaskResultAbi.IntTypeId, block);
        if (packed is null || shift is null || state is null
            || successState is null || succeeded is null || value is null) return false;
        instructions.Add(new("binary", state.Id, new[] { packed.Id, shift.Id },
            null, "right_shift", null));
        instructions.Add(new("binary", succeeded.Id, new[] { state.Id, successState.Id },
            null, "equals", null));
        instructions.Add(new("convert", value.Id, new[] { packed.Id }, null, null, null));
        return true;
    }
}
