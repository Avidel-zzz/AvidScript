using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal static class CSharpAsyncThrowLowerer
{
    public static GuestAsyncRaise Route(SemanticDocument document, SemanticAsyncMethod method,
        SemanticAsyncSegment segment)
    {
        var site = method.ErrorPlan!.Throws.Single(item => item.SegmentOrdinal == segment.Ordinal);
        var catalog = CSharpAsyncLanguageErrorCatalog.Build(document);
        return new(catalog.Types.Single(item => item.TypeId == site.ExceptionTypeId).Token,
            catalog.Sources.Single(item => item.SourceId == method.ErrorPlan.SourceId && item.Span == site.Span).Token);
    }

    public static bool Emit(CSharpFunctionLoweringContext context, SemanticAsyncMethod method,
        SemanticAsyncSegment segment, string blockId, List<GuestInstruction> instructions,
        List<GuestBasicBlock> blocks)
    {
        if (!SemanticContract.HasAsyncThrowRouting(context.Document)
            || segment.Transfer?.PrimaryTarget is not >= 0
            || !context.TryGetStorage(CSharpTaskResultAbi.ExceptionSourceSlot(method), out var owner)
            || !context.TryGetStorage(CSharpTaskResultAbi.ExceptionTypeSlot(method), out var typeStorage)) return false;
        string marker = blockId + ":raise_exception";
        blocks.Add(new(blockId, instructions, new("branch", null, marker, null, null)));
        int block = segment.Ordinal;
        List<GuestInstruction> create = new();
        var task = CSharpTaskResultAbi.Call(context, CSharpTaskResultAbi.Create, null, null, block, create);
        var zero = CSharpTaskResultAbi.Constant(context, "type:int64", 0, block, create);
        var created = context.CreateTemporary("type:int32", block);
        if (task is null || zero is null || created is null) return false;
        create.Add(new("binary", created.Id, new[] { task.Id, zero.Id }, null, "not_equals", null));
        string faultId = marker + ":task_created";
        string createRejected = marker + ":task_create_rejected";
        blocks.Add(new(marker, create, new("branch_if", created.Id, faultId, createRejected, null)));
        blocks.Add(new(createRejected, Array.Empty<GuestInstruction>(), new("trap", null, null, null, null)));

        GuestAsyncRaise route = Route(context.Document, method, segment);
        List<GuestInstruction> fault = new();
        var type = CSharpTaskResultAbi.Constant(context, "type:int32", route.TypeToken, block, fault);
        var source = CSharpTaskResultAbi.Constant(context, "type:int32", route.SourceToken, block, fault);
        var root = context.CreateTemporary("type:language_error_root", block);
        var accepted = context.CreateTemporary("type:int32", block);
        if (type is null || source is null || root is null || accepted is null) return false;
        int first = fault.Count;
        fault.Add(new("managed_new", root.Id, Array.Empty<string>(), null, null, null));
        fault.Add(new("managed_set", null, new[] { root.Id, type.Id }, "field:code", null, null));
        fault.Add(new("call", accepted.Id, new[] { task.Id, type.Id, source.Id, root.Id },
            CSharpTaskResultAbi.FaultLanguageErrorImportId, null, null));
        CSharpGuestDebugTagger.TagFirstEmitted(fault, first, segment.Span,
            CSharpGuestDebugTagger.OperationId(method.MethodSymbolId, $"async:{block}:throw", 0), "statement");
        string acquired = marker + ":fault_acquired";
        string rejected = marker + ":fault_rejected";
        blocks.Add(new(faultId, fault, new("branch_if", accepted.Id, acquired, rejected, null)));
        List<GuestInstruction> reject = new();
        if (CSharpTaskResultAbi.Call(context, CSharpTaskResultAbi.Release, task, null, block, reject) is null) return false;
        blocks.Add(new(rejected, reject, new("trap", null, null, null, null)));

        // Acquire the replacement first. Releasing the previous packet cannot
        // collect the new object, which is now rooted by the fresh faulted Task.
        List<GuestInstruction> replace = new();
        string active = acquired;
        if (!CSharpAsyncExceptionLowerer.ReleaseIfHeld(context, method, block, blocks, ref active, ref replace)) return false;
        string publish = marker + ":publish";
        blocks.Add(new(active, replace, new("branch", null, publish, null, null)));
        blocks.Add(new(publish, new GuestInstruction[]
        {
            new("local_store", null, new[] { task.Id }, owner.Id, null, null),
            new("local_store", null, new[] { type.Id }, typeStorage.Id, null, null),
        }, new("branch", null, CSharpGuestIds.AsyncSegmentBlock(method.MethodSymbolId,
            segment.Transfer.PrimaryTarget), null, null)));
        return true;
    }
}
