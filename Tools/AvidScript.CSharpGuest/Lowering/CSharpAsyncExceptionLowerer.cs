using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

// The source Task is the persistent owner of a language-error object. The
// routing functions run synchronously after one await; a future await inside
// catch/finally must also transfer these slots into its continuation frame.
internal static class CSharpAsyncExceptionLowerer
{
    public static bool Initialize(CSharpFunctionLoweringContext context,
        SemanticAsyncMethod method, int block, List<GuestInstruction> instructions)
    {
        if (!context.TryGetStorage(CSharpTaskResultAbi.ExceptionSourceSlot(method),
                out GuestRegister ownerStorage)
            || !context.TryGetStorage(CSharpTaskResultAbi.ExceptionTypeSlot(method),
                out GuestRegister typeStorage)) return false;
        GuestRegister? zeroTask = CSharpTaskResultAbi.Constant(context,
            CSharpTaskResultAbi.TokenTypeId, 0, block, instructions);
        GuestRegister? zeroType = CSharpTaskResultAbi.Constant(context,
            CSharpTaskResultAbi.IntTypeId, 0, block, instructions);
        if (zeroTask is null || zeroType is null) return false;
        instructions.Add(new("local_store", null, new[] { zeroTask.Id },
            ownerStorage.Id, null, null));
        instructions.Add(new("local_store", null, new[] { zeroType.Id },
            typeStorage.Id, null, null));
        return true;
    }

    public static bool EmitFailure(CSharpFunctionLoweringContext context,
        SemanticAsyncMethod method, SemanticAsyncControlTransfer transfer,
        GuestRegister sourceTask, GuestRegister state, int block,
        string blockId, List<GuestInstruction> instructions,
        List<GuestBasicBlock> blocks, bool releaseDirectToken)
    {
        if (method.ExceptionPlan is null || transfer.SecondaryTarget < 0
            || transfer.CancellationTarget is not int cancellationTarget
            || !context.TryGetStorage(CSharpTaskResultAbi.ExceptionSourceSlot(method),
                out GuestRegister ownerStorage)
            || !context.TryGetStorage(CSharpTaskResultAbi.ExceptionTypeSlot(method),
                out GuestRegister typeStorage)) return false;

        GuestRegister? faultState = CSharpTaskResultAbi.Constant(context,
            CSharpTaskResultAbi.TokenTypeId, 2, block, instructions);
        GuestRegister? isFault = context.CreateTemporary(CSharpTaskResultAbi.IntTypeId, block);
        if (faultState is null || isFault is null) return false;
        instructions.Add(new("binary", isFault.Id,
            new[] { state.Id, faultState.Id }, null, "equals", null));
        string faultBlock = blockId + ":fault";
        string otherBlock = blockId + ":not_fault";
        blocks.Add(new(blockId, instructions,
            new("branch_if", isFault.Id, faultBlock, otherBlock, null)));

        List<GuestInstruction> faultInstructions = new();
        CSharpTaskLanguageError? error = CSharpTaskResultAbi.ReadLanguageError(
            context, sourceTask, block, faultInstructions);
        if (error is null || !RetainAndRoute(context, sourceTask, block,
                faultBlock, faultInstructions, blocks, ownerStorage,
                CSharpGuestIds.AsyncSegmentBlock(method.MethodSymbolId,
                    transfer.SecondaryTarget), releaseDirectToken,
                typeStorage, error.TypeToken)) return false;

        List<GuestInstruction> otherInstructions = new();
        GuestRegister? cancelledState = CSharpTaskResultAbi.Constant(context,
            CSharpTaskResultAbi.TokenTypeId, 3, block, otherInstructions);
        GuestRegister? isCancelled = context.CreateTemporary(CSharpTaskResultAbi.IntTypeId, block);
        if (cancelledState is null || isCancelled is null) return false;
        otherInstructions.Add(new("binary", isCancelled.Id,
            new[] { state.Id, cancelledState.Id }, null, "equals", null));
        string cancelledBlock = blockId + ":cancelled";
        string invalidBlock = blockId + ":invalid_state";
        blocks.Add(new(otherBlock, otherInstructions,
            new("branch_if", isCancelled.Id, cancelledBlock, invalidBlock, null)));
        if (!RetainAndRoute(context, sourceTask, block, cancelledBlock,
                new List<GuestInstruction>(), blocks, ownerStorage,
                CSharpGuestIds.AsyncSegmentBlock(method.MethodSymbolId,
                    cancellationTarget), releaseDirectToken, null, null)) return false;
        List<GuestInstruction> invalidInstructions = new();
        if (releaseDirectToken && CSharpTaskResultAbi.Call(context,
                CSharpTaskResultAbi.Release, sourceTask, null, block,
                invalidInstructions) is null) return false;
        blocks.Add(new(invalidBlock, invalidInstructions,
            new("trap", null, null, null, null)));
        return true;
    }

    public static bool EmitCatchMatch(CSharpFunctionLoweringContext context,
        SemanticAsyncMethod method, SemanticAsyncControlTransfer transfer,
        int block, string blockId, List<GuestInstruction> instructions,
        List<GuestBasicBlock> blocks)
    {
        if (transfer.ExceptionTypeId is null
            || !context.TryGetStorage(CSharpTaskResultAbi.ExceptionTypeSlot(method),
                out GuestRegister typeStorage)) return false;
        CSharpLanguageErrorTypeToken? expected = CSharpAsyncLanguageErrorCatalog
            .Build(context.Document).Types.SingleOrDefault(item =>
                item.TypeId == transfer.ExceptionTypeId);
        if (expected is null) return false;
        GuestRegister? actual = context.CreateTemporary(CSharpTaskResultAbi.IntTypeId, block);
        GuestRegister? token = CSharpTaskResultAbi.Constant(context,
            CSharpTaskResultAbi.IntTypeId, expected.Token, block, instructions);
        GuestRegister? matches = context.CreateTemporary(CSharpTaskResultAbi.IntTypeId, block);
        if (actual is null || token is null || matches is null) return false;
        instructions.Add(new("local_load", actual.Id, Array.Empty<string>(),
            typeStorage.Id, null, null));
        instructions.Add(new("binary", matches.Id,
            new[] { actual.Id, token.Id }, null, "equals", null));
        blocks.Add(new(blockId, instructions,
            new("branch_if", matches.Id,
                CSharpGuestIds.AsyncSegmentBlock(method.MethodSymbolId,
                    transfer.PrimaryTarget),
                CSharpGuestIds.AsyncSegmentBlock(method.MethodSymbolId,
                    transfer.SecondaryTarget), null)));
        return true;
    }

    public static bool ReleaseIfHeld(CSharpFunctionLoweringContext context,
        SemanticAsyncMethod method, int block, List<GuestBasicBlock> blocks,
        ref string activeBlockId, ref List<GuestInstruction> instructions)
    {
        if (method.ExceptionPlan is null) return true;
        GuestRegister? owner = LoadOwner(context, method, block, instructions);
        GuestRegister? zero = CSharpTaskResultAbi.Constant(context,
            CSharpTaskResultAbi.TokenTypeId, 0, block, instructions);
        GuestRegister? hasOwner = context.CreateTemporary(CSharpTaskResultAbi.IntTypeId, block);
        if (owner is null || zero is null || hasOwner is null) return false;
        instructions.Add(new("binary", hasOwner.Id,
            new[] { owner.Id, zero.Id }, null, "not_equals", null));
        string releaseBlock = activeBlockId + ":release_error_owner";
        string nextBlock = activeBlockId + ":owner_released";
        blocks.Add(new(activeBlockId, instructions,
            new("branch_if", hasOwner.Id, releaseBlock, nextBlock, null)));
        List<GuestInstruction> release = new();
        GuestRegister? released = CSharpTaskResultAbi.Call(context,
            CSharpTaskResultAbi.Release, owner, null, block, release);
        GuestRegister? releaseZero = CSharpTaskResultAbi.Constant(context,
            CSharpTaskResultAbi.TokenTypeId, 0, block, release);
        GuestRegister? accepted = context.CreateTemporary(CSharpTaskResultAbi.IntTypeId, block);
        if (released is null || releaseZero is null || accepted is null) return false;
        release.Add(new("binary", accepted.Id,
            new[] { released.Id, releaseZero.Id }, null, "not_equals", null));
        string rejectedBlock = releaseBlock + ":rejected";
        blocks.Add(new(releaseBlock, release,
            new("branch_if", accepted.Id, nextBlock, rejectedBlock, null)));
        blocks.Add(new(rejectedBlock, Array.Empty<GuestInstruction>(),
            new("trap", null, null, null, null)));
        activeBlockId = nextBlock;
        instructions = new List<GuestInstruction>();
        return true;
    }

    public static GuestRegister? LoadOwner(CSharpFunctionLoweringContext context,
        SemanticAsyncMethod method, int block, List<GuestInstruction> instructions)
    {
        if (!context.TryGetStorage(CSharpTaskResultAbi.ExceptionSourceSlot(method),
                out GuestRegister storage)) return null;
        GuestRegister? owner = context.CreateTemporary(CSharpTaskResultAbi.TokenTypeId, block);
        if (owner is not null)
            instructions.Add(new("local_load", owner.Id, Array.Empty<string>(),
                storage.Id, null, null));
        return owner;
    }

    private static bool RetainAndRoute(CSharpFunctionLoweringContext context,
        GuestRegister sourceTask, int block, string blockId,
        List<GuestInstruction> instructions, List<GuestBasicBlock> blocks,
        GuestRegister ownerStorage, string targetBlockId, bool releaseDirectToken,
        GuestRegister? typeStorage, GuestRegister? typeToken)
    {
        GuestRegister? retained = CSharpTaskResultAbi.Call(context,
            CSharpTaskResultAbi.Retain, sourceTask, null, block, instructions);
        GuestRegister? zero = CSharpTaskResultAbi.Constant(context,
            CSharpTaskResultAbi.TokenTypeId, 0, block, instructions);
        GuestRegister? accepted = context.CreateTemporary(CSharpTaskResultAbi.IntTypeId, block);
        if (retained is null || zero is null || accepted is null) return false;
        instructions.Add(new("binary", accepted.Id,
            new[] { retained.Id, zero.Id }, null, "not_equals", null));
        string acceptedBlock = blockId + ":retained";
        string rejectedBlock = blockId + ":retain_rejected";
        blocks.Add(new(blockId, instructions,
            new("branch_if", accepted.Id, acceptedBlock, rejectedBlock, null)));
        List<GuestInstruction> owned = new()
        {
            new("local_store", null, new[] { sourceTask.Id },
                ownerStorage.Id, null, null),
        };
        if (typeStorage is not null && typeToken is not null)
            owned.Add(new("local_store", null, new[] { typeToken.Id },
                typeStorage.Id, null, null));
        if (releaseDirectToken)
        {
            GuestRegister? released = CSharpTaskResultAbi.Call(context,
                CSharpTaskResultAbi.Release, sourceTask, null, block, owned);
            GuestRegister? releaseZero = CSharpTaskResultAbi.Constant(context,
                CSharpTaskResultAbi.TokenTypeId, 0, block, owned);
            GuestRegister? releaseAccepted = context.CreateTemporary(
                CSharpTaskResultAbi.IntTypeId, block);
            if (released is null || releaseZero is null || releaseAccepted is null)
                return false;
            owned.Add(new("binary", releaseAccepted.Id,
                new[] { released.Id, releaseZero.Id }, null, "not_equals", null));
            string releaseRejected = acceptedBlock + ":release_rejected";
            blocks.Add(new(acceptedBlock, owned,
                new("branch_if", releaseAccepted.Id, targetBlockId,
                    releaseRejected, null)));
            blocks.Add(new(releaseRejected, Array.Empty<GuestInstruction>(),
                new("trap", null, null, null, null)));
        }
        else
        {
            blocks.Add(new(acceptedBlock, owned,
                new("branch", null, targetBlockId, null, null)));
        }
        List<GuestInstruction> rejected = new();
        if (releaseDirectToken && CSharpTaskResultAbi.Call(context,
                CSharpTaskResultAbi.Release, sourceTask, null, block,
                rejected) is null) return false;
        blocks.Add(new(rejectedBlock, rejected,
            new("trap", null, null, null, null)));
        return true;
    }
}
