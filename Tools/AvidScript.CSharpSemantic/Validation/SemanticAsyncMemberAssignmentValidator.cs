using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;

namespace AvidScript.CSharpSemantic;

// Validate the capture/writeback protocol independently of the projector.
// Member selection remains a normal source operation; only its receiver is saved.
public static class SemanticAsyncMemberAssignmentValidator
{
    public static bool IsValid(SemanticDocument document)
    {
        if (document?.AsyncMethods is null || document.Methods is null || document.Symbols is null
            || document.Types is null || document.Callables is null) return false;
        bool enabled = SemanticContract.HasAsyncMemberAssignments(document);
        bool found = false;
        foreach (SemanticAsyncMethod method in document.AsyncMethods)
        {
            if (method?.Segments is null || method.CompilerLocals is null) return false;
            string prefix = SemanticAsyncMemberAssignment.LocalPrefix(method.MethodSymbolId);
            var expectedLocals = new Dictionary<string, SemanticAsyncCompilerLocal>(StringComparer.Ordinal);
            foreach (SemanticAsyncSegment segment in method.Segments)
            {
                if (segment is null) return false;
                SemanticAsyncAwaitSite? site = segment.AwaitSite;
                if (site?.MemberAssignment is not { } plan)
                {
                    if (site?.ResultStorageKind == "member_assignment") return false;
                    continue;
                }
                found = true;
                if (!enabled || !ValidateSite(document, method, segment, site, plan, expectedLocals))
                    return false;
            }
            var declared = method.CompilerLocals.Where(local => local?.SymbolId?.StartsWith(prefix,
                StringComparison.Ordinal) == true).ToArray();
            if (declared.Length != expectedLocals.Count || declared.Any(local =>
                !expectedLocals.TryGetValue(local.SymbolId, out var expected) || local != expected)
                || declared.Select(local => local.SymbolId).Distinct(StringComparer.Ordinal).Count() != declared.Length)
                return false;
            if (expectedLocals.Count != 0)
            {
                if (method.CompilerLocals.Any(local => local is null)
                    || method.CompilerLocals.Select(local => local.SymbolId).Distinct(StringComparer.Ordinal).Count()
                        != method.CompilerLocals.Count
                    || expectedLocals.Keys.Any(id => document.Symbols.Any(symbol => symbol?.Id == id))) return false;
                SemanticAsyncStateFlowAnalysis flow = SemanticAsyncStateFlowAnalyzer.AnalyzeControlFlow(method.Segments);
                if (flow.Issues.Count != 0) return false;
                foreach (var segment in method.Segments.Where(segment => segment.AwaitSite is not null))
                {
                    var expectedSlots = (flow.SlotsByAwaitSegment.GetValueOrDefault(segment.Ordinal)
                            ?? Array.Empty<SemanticAsyncStateSlot>()).Concat(method.InvocationInputs)
                        .Distinct().OrderBy(slot => slot.SymbolId, StringComparer.Ordinal).ToArray();
                    if (!expectedSlots.SequenceEqual(segment.AwaitSite!.StateFrame?.Slots
                        ?? Array.Empty<SemanticAsyncStateSlot>())) return false;
                }
            }
        }
        return enabled ? found : document.SchemaVersion != SemanticContract.AsyncMemberAssignmentSchemaVersion
            && document.SemanticVersion != SemanticContract.AsyncMemberAssignmentSemanticVersion;
    }

    private static bool ValidateSite(SemanticDocument document, SemanticAsyncMethod method,
        SemanticAsyncSegment segment, SemanticAsyncAwaitSite site, SemanticAsyncMemberAssignment plan,
        IDictionary<string, SemanticAsyncCompilerLocal> locals)
    {
        if (method.Lowering != SemanticAsyncMethod.ContinuationCfgLowering
            || method.Segments.Where((item, index) => item is null || item.Ordinal != index
                || item.Transfer is null || item.Statements is null).Any()
            || site.ProducerKind is not ("task_call" or "task_local")
            || site.ResultStorageKind != "member_assignment" || site.ResultTypeId != "type:int32"
            || site.PayloadKind != "task_result" || site.PayloadValueTypeId != "type:int32"
            || site.Span is null || site.Span.Length <= 0
            || plan.Target is not { IsSupported: true, TypeId: "type:int32", Children.Count: 1 } target
            || target.Kind is not ("field_reference" or "property_reference")
            || target.Children[0] is not { IsSupported: true, TypeId: { } receiverType, Span: { } receiverSpan } receiver
            || !document.Types.Any(type => type?.Id == receiverType && type.Kind is "class" or "interface")
            || !Contains(segment.Span, site.Span) || !Contains(segment.Span, target.Span)
            || !Contains(target.Span, receiverSpan)
            || plan.ReceiverSymbolId != SemanticAsyncMemberAssignment.ReceiverSymbol(method.MethodSymbolId, site.Span.Start)
            || site.ResultSymbolId != SemanticAsyncMemberAssignment.ResultSymbol(method.MethodSymbolId, site.Span.Start)
            || !locals.TryAdd(plan.ReceiverSymbolId, new(plan.ReceiverSymbolId,
                SemanticAsyncMemberAssignment.ReceiverName(site.Span.Start), receiverType, receiverSpan))
            || !locals.TryAdd(site.ResultSymbolId, new(site.ResultSymbolId,
                SemanticAsyncMemberAssignment.ResultName(site.Span.Start), "type:int32", site.Span))) return false;

        SemanticSymbol[] members = document.Symbols.Where(symbol => symbol?.Id == target.SymbolId).ToArray();
        if (members.Length != 1 || members[0].IsStatic || members[0].IsConst || members[0].IsReadonly
            || members[0].TypeId != "type:int32"
            || members[0].Kind != (target.Kind == "field_reference" ? "field" : "property")) return false;
        if (target.Kind == "property_reference" && !document.Callables.Any(callable => callable is
            { IsStatic: false, IsConstructor: false, ReturnTypeId: "type:void", Parameters.Count: 1 }
            && callable.AssociatedSymbolId == target.SymbolId
            && callable.Parameters[0] is { TypeId: "type:int32", RefKind: "none" })) return false;

        // The original method tree must contain the very same LHS and await.
        var bodies = document.Methods.Where(body => body?.MethodSymbolId == method.MethodSymbolId).ToArray();
        if (bodies.Length != 1 || bodies[0].Root is null
            || !Operations(bodies[0].Root).Any(operation => operation is
                { Kind: "assignment", IsSupported: true, TypeId: "type:int32", Children.Count: 2 }
                && Same(operation.Children[0], SourceIdentities(document, target))
                && operation.Children[1] is { Kind: "await", TypeId: "type:int32" } awaited
                && awaited.Span == site.Span)) return false;

        if (segment.Statements.Count != 1
            || segment.Statements[0].TargetSymbolId != plan.ReceiverSymbolId
            || !Same(segment.Statements[0].Operation, receiver)
            || segment.Transfer is not { Kind: SemanticAsyncMethod.AwaitTransferKind } transfer
            || transfer.PrimaryTarget != plan.WriteSegmentOrdinal
            || plan.WriteSegmentOrdinal < 0 || plan.WriteSegmentOrdinal >= method.Segments.Count
            || plan.WriteSegmentOrdinal == method.EntrySegmentOrdinal) return false;
        SemanticAsyncSegment write = method.Segments[plan.WriteSegmentOrdinal];
        if (write.AwaitSite is not null || write.Span != segment.Span
            || write.Transfer is not { Kind: SemanticAsyncMethod.GotoTransferKind, Condition: null,
                SecondaryTarget: -1, CancellationTarget: null, ExceptionTypeId: null } writeTransfer
            || writeTransfer.PrimaryTarget < 0 || writeTransfer.PrimaryTarget >= method.Segments.Count
            || writeTransfer.PrimaryTarget == write.Ordinal
            || write.Statements.Count != 1 || write.Statements[0].TargetSymbolId is not null) return false;

        var receiverLocal = Local(plan.ReceiverSymbolId, receiverType, receiverSpan);
        var resultLocal = Local(site.ResultSymbolId, "type:int32", site.Span);
        var expectedWrite = new SemanticOperation("assignment", true, null, false, false, false, false,
            "type:int32", null, Array.Empty<string>(), null, null, null, null, null, segment.Span,
            new[] { target with { Children = new[] { receiverLocal } }, resultLocal });
        if (!Same(write.Statements[0].Operation, expectedWrite)) return false;

        // Only the normal completion edge may enter the write. Neither an error
        // edge nor a loop/branch may skip the capture and producer invocation.
        int incoming = 0;
        foreach (var candidate in method.Segments)
        {
            var edge = candidate.Transfer!;
            if (edge.PrimaryTarget == write.Ordinal)
            {
                if (candidate.Ordinal != segment.Ordinal) return false;
                incoming++;
            }
            if (edge.SecondaryTarget == write.Ordinal || edge.CancellationTarget == write.Ordinal) return false;
            foreach (var statement in candidate.Statements)
            {
                if (statement.TargetSymbolId == site.ResultSymbolId
                    || statement.TargetSymbolId == plan.ReceiverSymbolId && candidate.Ordinal != segment.Ordinal)
                    return false;
            }
            var operations = candidate.Statements.SelectMany(statement => Operations(statement.Operation))
                .Concat(edge.Condition is null ? Array.Empty<SemanticOperation>() : Operations(edge.Condition))
                .Concat(candidate.AwaitSite?.Arguments.SelectMany(Operations) ?? Array.Empty<SemanticOperation>())
                .Concat(candidate.AwaitSite?.CancellationToken is { } token
                    ? Operations(token) : Array.Empty<SemanticOperation>());
            if (operations.Any(operation => (operation.SymbolId == plan.ReceiverSymbolId
                    || operation.SymbolId == site.ResultSymbolId)
                && candidate.Ordinal != write.Ordinal)) return false;
        }
        return incoming == 1;
    }

    private static SemanticOperation Local(string id, string type, SemanticSpan span) =>
        new("local_reference", true, null, false, false, false, false, type, id,
            Array.Empty<string>(), null, null, null, null, null, span, Array.Empty<SemanticOperation>());

    private static bool Contains(SemanticSpan? outer, SemanticSpan? inner) =>
        outer is not null && inner is not null && inner.Start >= outer.Start && inner.Length >= 0
        && (long)inner.Start + inner.Length <= (long)outer.Start + outer.Length;

    private static bool Same(SemanticOperation? left, SemanticOperation? right) =>
        left is not null && right is not null && JsonSerializer.Serialize(left) == JsonSerializer.Serialize(right);

    private static SemanticOperation SourceIdentities(SemanticDocument document, SemanticOperation operation)
    {
        string? sourceSymbol = operation.SymbolId;
        var instance = document.Callables.FirstOrDefault(callable => callable is not null && callable.MethodSymbolId == sourceSymbol
            && callable.GenericDefinitionSymbolId is not null);
        // Generic specialization preserves the source method tree. Translate
        // only a verified closed identity back to that tree for comparison.
        if (instance?.GenericDefinitionSymbolId is { } definition
            && instance.GenericArgumentTypeIds is { } arguments
            && arguments.SequenceEqual(operation.TypeArgumentIds)
            && sourceSymbol == SemanticContract.GenericInstanceId(definition, arguments))
            sourceSymbol = definition;
        return operation with
        {
            SymbolId = sourceSymbol,
            Children = operation.Children.Select(child => SourceIdentities(document, child)).ToArray(),
        };
    }

    private static IEnumerable<SemanticOperation> Operations(SemanticOperation root)
    {
        yield return root;
        foreach (var child in root.Children)
            foreach (var operation in Operations(child)) yield return operation;
    }
}
