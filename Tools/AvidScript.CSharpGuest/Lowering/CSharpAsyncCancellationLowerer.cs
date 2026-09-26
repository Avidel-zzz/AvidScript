using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal static class CSharpAsyncCancellationLowerer
{
    // An unprotected await still faults its returned Task on cancellation. The
    // source already supplies the producer, token and span; no synthetic source
    // try/catch or Roslyn region is needed to lower that terminal operation.
    public static bool NeedsImplicitPropagation(SemanticDocument document,
        SemanticAsyncMethod method, SemanticAsyncSegment segment) =>
        CSharpTaskResultAbi.SupportsCancellation(document) && method.TaskResultTypeId is not null
        && segment.AwaitSite?.ProducerKind is "delay" or "next_tick"
        && segment.Transfer?.CancellationTarget is null;

    public static bool IsStatusAware(SemanticDocument document,
        SemanticAsyncMethod method, SemanticAsyncSegment segment) =>
        segment.AwaitSite?.ProducerKind is "delay" or "next_tick"
        && (segment.Transfer?.CancellationTarget is >= 0 || NeedsImplicitPropagation(document, method, segment));

    public static bool HasExceptionStorage(SemanticDocument document, SemanticAsyncMethod method) =>
        method.ExceptionPlan is not null || method.Segments.Any(segment => NeedsImplicitPropagation(document, method, segment));

    public static string ImplicitPropagationBlock(SemanticAsyncMethod method, SemanticAsyncAwaitSite site) =>
        CSharpGuestIds.Function(method.MethodSymbolId) + ":cancel_at:" + site.CallbackId;

    public static IReadOnlyList<GuestAsyncExceptionTransfer> Transfers(SemanticDocument document) =>
        document.AsyncMethods.Where(method => method.ExceptionPlan is not null)
            .SelectMany(method => method.Segments.Where(segment => segment.Transfer?.Kind is
                SemanticAsyncMethod.EndCatchTransferKind or SemanticAsyncMethod.RethrowTransferKind
                    or SemanticAsyncMethod.PropagateExceptionTransferKind or SemanticAsyncMethod.RaiseExceptionTransferKind)
                .Select(segment => new GuestAsyncExceptionTransfer(
                    CSharpGuestIds.Function(method.MethodSymbolId),
                    CSharpGuestIds.AsyncSegmentBlock(method.MethodSymbolId, segment.Ordinal),
                    segment.Transfer!.Kind,
                    segment.Transfer.PrimaryTarget >= 0
                        ? CSharpGuestIds.AsyncSegmentBlock(method.MethodSymbolId, segment.Transfer.PrimaryTarget) : null,
                    CSharpGuestIds.Local(CSharpTaskResultAbi.ExceptionSourceSlot(method)),
                    CSharpGuestIds.Local(CSharpTaskResultAbi.ExceptionTypeSlot(method)))
                    {
                        Raise = segment.Transfer.Kind == SemanticAsyncMethod.RaiseExceptionTransferKind
                            ? CSharpAsyncThrowLowerer.Route(document, method, segment) : null,
                    }))
            .Concat(document.AsyncMethods.SelectMany(method => method.Segments
                .Where(segment => NeedsImplicitPropagation(document, method, segment))
                .Select(segment => new GuestAsyncExceptionTransfer(
                    CSharpGuestIds.Function(method.MethodSymbolId), ImplicitPropagationBlock(method, segment.AwaitSite!),
                    SemanticAsyncMethod.PropagateExceptionTransferKind, null,
                    CSharpGuestIds.Local(CSharpTaskResultAbi.ExceptionSourceSlot(method)),
                    CSharpGuestIds.Local(CSharpTaskResultAbi.ExceptionTypeSlot(method))))))
            .OrderBy(transfer => transfer.MethodFunctionId, StringComparer.Ordinal)
            .ThenBy(transfer => transfer.BlockId, StringComparer.Ordinal).ToArray();

    public static GuestDirectAwaitCancellation Route(SemanticDocument document,
        SemanticAsyncMethod method, SemanticAsyncAwaitSite site)
    {
        CSharpLanguageErrorTokenCatalog catalog = CSharpAsyncLanguageErrorCatalog.Build(document);
        return new(
            CSharpGuestIds.Local(CSharpTaskResultAbi.ExceptionSourceSlot(method)),
            CSharpGuestIds.Local(CSharpTaskResultAbi.ExceptionTypeSlot(method)),
            catalog.Types.Single(item => item.TypeId == (method.ExceptionPlan?.CancellationTypeId
                ?? SemanticAsyncCancellationPlanValidator.CancellationTypeId)).Token,
            catalog.Sources.Single(item => item.SourceId == document.Source.SourceId
                && item.Span == site.Span).Token);
    }

    // A Timer has no source Task. Give its cancellation object one owned Task
    // lease so catch, rethrow and Task propagation use the same representation.
    public static bool EmitDirect(CSharpFunctionLoweringContext context,
        SemanticAsyncMethod method, SemanticAsyncAwaitSite site, int block,
        string blockId, string target, List<GuestInstruction> instructions,
        List<GuestBasicBlock> blocks)
    {
        GuestDirectAwaitCancellation route = Route(context.Document, method, site);
        if (!context.TryGetStorage(CSharpTaskResultAbi.ExceptionSourceSlot(method), out GuestRegister owner)
            || !context.TryGetStorage(CSharpTaskResultAbi.ExceptionTypeSlot(method), out GuestRegister typeStorage))
            return false;
        GuestRegister? task = CSharpTaskResultAbi.Call(context, CSharpTaskResultAbi.Create,
            null, null, block, instructions);
        GuestRegister? zero = CSharpTaskResultAbi.Constant(context, "type:int64", 0, block, instructions);
        GuestRegister? created = context.CreateTemporary("type:int32", block);
        if (task is null || zero is null || created is null) return false;
        instructions.Add(new("binary", created.Id, new[] { task.Id, zero.Id }, null, "not_equals", null));
        string createAccepted = blockId + ":task_created";
        string createRejected = blockId + ":task_create_rejected";
        blocks.Add(new(blockId, instructions, new("branch_if", created.Id, createAccepted, createRejected, null)));
        blocks.Add(new(createRejected, Array.Empty<GuestInstruction>(), new("trap", null, null, null, null)));

        List<GuestInstruction> cancel = new();
        GuestRegister? type = CSharpTaskResultAbi.Constant(context, "type:int32", route.TypeToken, block, cancel);
        GuestRegister? source = CSharpTaskResultAbi.Constant(context, "type:int32", route.SourceToken, block, cancel);
        GuestRegister? root = context.CreateTemporary("type:language_error_root", block);
        GuestRegister? accepted = context.CreateTemporary("type:int32", block);
        if (type is null || source is null || root is null || accepted is null) return false;
        cancel.Add(new("managed_new", root.Id, Array.Empty<string>(), null, null, null));
        cancel.Add(new("managed_set", null, new[] { root.Id, type.Id }, "field:code", null, null));
        cancel.Add(new("call", accepted.Id, new[] { task.Id, type.Id, source.Id, root.Id },
            GuestTaskCancellationErrorValidator.ImportId, null, null));
        string cancelledOwner = blockId + ":cancelled_owner";
        string cancelRejected = blockId + ":cancel_rejected";
        blocks.Add(new(createAccepted, cancel, new("branch_if", accepted.Id, cancelledOwner, cancelRejected, null)));
        blocks.Add(new(cancelledOwner, new GuestInstruction[]
        {
            new("local_store", null, new[] { task.Id }, owner.Id, null, null),
            new("local_store", null, new[] { type.Id }, typeStorage.Id, null, null),
        }, new("branch", null, target, null, null)));
        List<GuestInstruction> rejected = new();
        if (CSharpTaskResultAbi.Call(context, CSharpTaskResultAbi.Release, task, null, block, rejected) is null)
            return false;
        blocks.Add(new(cancelRejected, rejected, new("trap", null, null, null, null)));
        return true;
    }
}
