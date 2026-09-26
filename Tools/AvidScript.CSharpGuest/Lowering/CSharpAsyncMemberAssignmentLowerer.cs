using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

// Capture and writeback remain ordinary source operations. Only the implicit
// null failure needs an outcome producer, evaluated after a successful await.
internal static class CSharpAsyncMemberAssignmentLowerer
{
    internal const string NullReferenceType = "type:global::System.NullReferenceException";
    private const string Int = "type:int32";
    private const string Root = "type:language_error_root";
    private static string GuardId(SemanticAsyncMethod method, SemanticAsyncAwaitSite site) =>
        "function:$async:member_receiver:" + method.MethodSymbolId + ":" + site.CallbackId;
    private static string TaskGuardId(string method, int start) => "function:$async:task_receiver:" + method + ":" + start;
    internal sealed record GuardSite(string Id, string ReceiverType, SemanticSpan Span);

    internal static IReadOnlyList<GuardSite> GuardSites(SemanticDocument source)
    {
        var sites = new List<GuardSite>();
        var producers = source.AsyncMethods.Select(method => method.MethodSymbolId).ToHashSet(StringComparer.Ordinal);
        var calls = source.Callables.Where(callable => !callable.IsStatic && producers.Contains(callable.MethodSymbolId))
            .ToDictionary(callable => callable.MethodSymbolId, StringComparer.Ordinal);
        foreach (var method in source.AsyncMethods)
        foreach (var segment in method.Segments)
        {
            if (segment.AwaitSite is { MemberAssignment: { } plan } member)
                sites.Add(new(GuardId(method, member), method.CompilerLocals.Single(local => local.SymbolId == plan.ReceiverSymbolId).TypeId,
                    plan.Target.Span));
            if (segment.AwaitSite is { ProducerKind: "task_call", TaskCallableId: { } id } task && calls.TryGetValue(id, out var target))
                sites.Add(new(TaskGuardId(method.MethodSymbolId, task.Span.Start), target.ContainingTypeId, task.Span));
            foreach (var operation in segment.Statements.SelectMany(statement => Operations(statement.Operation)))
                if (operation.Kind == "invocation" && operation.SymbolId is { } called && calls.TryGetValue(called, out var producer))
                    sites.Add(new(TaskGuardId(method.MethodSymbolId, operation.Span.Start), producer.ContainingTypeId, operation.Span));
        }
        return sites.Distinct().OrderBy(site => site.Id, StringComparer.Ordinal).ToArray();
    }

    internal static bool CheckTaskReceiver(CSharpFunctionLoweringContext context, SemanticAsyncMethod method,
        SemanticAsyncAwaitSite site, string receiver, List<GuestInstruction> instructions) =>
        EmitTaskGuard(context, TaskGuardId(method.MethodSymbolId, site.Span.Start), receiver, instructions);

    internal static bool CheckInvocationReceiver(CSharpFunctionLoweringContext context, SemanticOperation operation,
        SemanticCallable target, IReadOnlyList<string> operands, List<GuestInstruction> instructions)
    {
        if (target.IsStatic || CSharpAsyncSynchronousExecutionContext.Find(context.Document) is null
            || !context.Document.AsyncMethods.Any(method => method.MethodSymbolId == target.MethodSymbolId)) return true;
        return operands.Count > 0 && EmitTaskGuard(context,
            TaskGuardId(context.Callable.MethodSymbolId, operation.Span.Start), operands[0], instructions);
    }

    private static bool EmitTaskGuard(CSharpFunctionLoweringContext context, string guardId,
        string receiver, List<GuestInstruction> instructions)
    {
        var execution = CSharpAsyncSynchronousExecutionContext.Find(context.Document);
        if (execution?.MemberGuards.Any(guard => guard.Id == guardId) != true)
        {
            context.Add("ASCG1026", "Instance Task invocation has no validated synchronous receiver guard.");
            return false;
        }
        instructions.Add(new("call", null, new[] { receiver }, guardId, null, null));
        return true;
    }

    internal static bool CheckReceiver(CSharpFunctionLoweringContext context, SemanticAsyncMethod method,
        SemanticAsyncSegment segment, List<GuestInstruction> instructions)
    {
        var site = method.Segments.Select(item => item.AwaitSite)
            .SingleOrDefault(site => site?.MemberAssignment?.WriteSegmentOrdinal == segment.Ordinal);
        if (site?.MemberAssignment is not { } plan) return true;
        if (CSharpAsyncSynchronousExecutionContext.Find(context.Document) is null
            || !context.TryGetStorage(plan.ReceiverSymbolId, out var storage)
            || !context.TryGetGuestType(storage.TypeId, out var type) || type.Kind != "managed_ref")
        {
            context.Add("ASCG1026", "Member await writeback requires a validated managed receiver and synchronous exception route.");
            return false;
        }
        var receiver = context.CreateTemporary(storage.TypeId, segment.Ordinal);
        if (receiver is null) return false;
        instructions.Add(new("local_load", receiver.Id, Array.Empty<string>(), storage.Id, null, null));
        instructions.Add(new("call", null, new[] { receiver.Id }, GuardId(method, site), null, null));
        return true;
    }

    internal static IReadOnlyList<GuestFunction> BuildGuards(SemanticDocument source, CSharpLanguageErrorTokenCatalog catalog)
    {
        var result = new List<GuestFunction>();
        foreach (var guard in GuardSites(source))
        {
            string receiverType = guard.ReceiverType;
            int type = catalog.Types.Single(item => item.TypeId == NullReferenceType).Token;
            int location = catalog.Sources.Single(item => item.SourceId == source.Source.SourceId && item.Span == guard.Span).Token;
            string outcome = CSharpLanguageOutcomeTypes.Id("type:void");
            result.Add(new(guard.Id, new[] { new GuestRegister("receiver", receiverType) }, new[] {
                new GuestRegister("null", receiverType), new GuestRegister("is_null", Int),
                new GuestRegister("zero", Int), new GuestRegister("one", Int),
                new GuestRegister("type", Int), new GuestRegister("source", Int), new GuestRegister("root", Root),
                new GuestRegister("success", outcome), new GuestRegister("failure", outcome),
            }, outcome, "entry", new[] {
                new GuestBasicBlock("entry", new GuestInstruction[] {
                    new("constant", "null", Array.Empty<string>(), null, null, new("null", null)),
                    new("binary", "is_null", new[] { "receiver", "null" }, null, "equals", null),
                }, new("branch_if", "is_null", "failed", "ok", null)),
                new GuestBasicBlock("ok", new[] {
                    Constant("zero", 0), Op("stack_alloc", "success"), Store("success", "status", "zero"),
                }, new("return", null, null, null, "success")),
                new GuestBasicBlock("failed", new[] {
                    Constant("one", 1), Constant("type", type), Constant("source", location),
                    Op("managed_new", "root"), Op("managed_set", operands: new[] { "root", "type" }, target: "field:code"),
                    Op("stack_alloc", "failure"), Store("failure", "status", "one"),
                    Store("failure", "error_type", "type"), Store("failure", "source", "source"), Store("failure", "error_root", "root"),
                }, new("return", null, null, null, "failure")),
            }));
        }
        return result.OrderBy(function => function.Id, StringComparer.Ordinal).ToArray();
    }

    private static IEnumerable<SemanticOperation> Operations(SemanticOperation operation)
    {
        yield return operation;
        foreach (var child in operation.Children)
            foreach (var nested in Operations(child)) yield return nested;
    }

    private static GuestInstruction Op(string op, string? result = null, string[]? operands = null, string? target = null) =>
        new(op, result, operands ?? Array.Empty<string>(), target, null, null);
    private static GuestInstruction Store(string owner, string field, string value) =>
        Op("field_store", operands: new[] { owner, value }, target: "field:" + field);
    private static GuestInstruction Constant(string id, int value) =>
        new("constant", id, Array.Empty<string>(), null, null, new("int32", value.ToString(CultureInfo.InvariantCulture)));
}
