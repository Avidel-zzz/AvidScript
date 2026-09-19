using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.CompilerServices;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal static class CSharpBoundDelegateLowerer
{
    private sealed record Plan(IReadOnlySet<string> Methods);
    private static readonly ConditionalWeakTable<SemanticDocument, Plan> Plans = new();
    public const string ReceiverField = "$receiver";
    public static string Box(string method) => "$delegate:box:" + method;
    public static IReadOnlySet<string> Methods(SemanticDocument document) => Plans.GetValue(document, Analyze).Methods;

    private static Plan Analyze(SemanticDocument document)
    {
        var callables = document.Callables.ToDictionary(callable => callable.MethodSymbolId, StringComparer.Ordinal);
        var structs = document.Types.Where(type => type.Kind == "struct" && type.IsValueType).Select(type => type.Id).ToHashSet(StringComparer.Ordinal);
        HashSet<string> used = new(StringComparer.Ordinal);
        Stack<SemanticOperation> pending = new(document.Methods.Select(method => method.Root));
        foreach (SemanticBasicBlock block in document.ControlFlowGraphs.SelectMany(graph => graph.Blocks))
        {
            foreach (SemanticOperation operation in block.Operations) pending.Push(operation);
            if (block.BranchValue is not null) pending.Push(block.BranchValue);
        }
        while (pending.TryPop(out SemanticOperation? operation))
        {
            if (operation.Kind == "delegate_creation" && operation.Children.Count == 1
                && operation.Children[0] is { Kind: "method_reference", Children.Count: 1, SymbolId: { } id }
                && callables.TryGetValue(id, out SemanticCallable? method) && !method.IsStatic && !method.IsConstructor
                && method.HasBody && method.Import is null && structs.Contains(method.ContainingTypeId)) used.Add(id);
            foreach (SemanticOperation child in operation.Children) pending.Push(child);
        }
        return new(used);
    }

    public static void AddTypes(SemanticDocument document, List<GuestType> types)
    {
        foreach (SemanticCallable callable in document.Callables.Where(callable => Methods(document).Contains(callable.MethodSymbolId))
                     .OrderBy(callable => callable.MethodSymbolId, StringComparer.Ordinal))
        {
            string box = Box(callable.MethodSymbolId), payload = CSharpClosureLayout.Payload(box);
            types.Add(new(payload, "struct", "memory", new[] { new GuestField(ReceiverField, ReceiverField, callable.ContainingTypeId, 0) }, null, null, 0, 1));
            types.Add(new(CSharpClosureLayout.Reference(box), "managed_ref", "i64", Array.Empty<GuestField>(), payload, null, 8, 8));
        }
    }

    public static GuestRegister? CreateContext(CSharpFunctionLoweringContext context, SemanticCallable callable,
        SemanticOperation target, int block, List<GuestInstruction> instructions)
    {
        if (target.Children.Count != 1 || target.Children[0].TypeId != callable.ContainingTypeId
            || !Methods(context.Document).Contains(callable.MethodSymbolId)
            || !context.TryGetGuestType(callable.ContainingTypeId, out GuestType type) || type.Kind != "struct")
        { context.Add("ASCG1024", "Bound delegate receiver requires a supported exact Guest struct instance method."); return null; }
        GuestRegister? receiver = CSharpOperationLowerer.LowerValue(context, target.Children[0], block, instructions);
        if (receiver is null) return null;
        GuestRegister box = context.CreateTemporary(CSharpClosureLayout.Reference(Box(callable.MethodSymbolId)), block)!;
        GuestRegister erased = context.CreateTemporary(CSharpClosureLayout.ObjectType, block)!;
        instructions.Add(new("managed_new", box.Id, Array.Empty<string>(), null, null, null));
        instructions.Add(new("managed_set", null, new[] { box.Id, receiver.Id }, ReceiverField, null, null));
        instructions.Add(new("managed_cast", erased.Id, new[] { box.Id }, null, null, null));
        return erased;
    }

    public static string ThunkReceiver(SemanticCallable callable, List<GuestRegister> locals, List<GuestInstruction> instructions)
    {
        locals.Add(new("receiver:box", CSharpClosureLayout.Reference(Box(callable.MethodSymbolId))));
        locals.Add(new("receiver:borrow", CSharpBorrowedReferences.Type(callable.ContainingTypeId)));
        instructions.Add(new("managed_cast", "receiver:box", new[] { "context" }, null, null, null));
        instructions.Add(new("borrow_managed", "receiver:borrow", new[] { "receiver:box" }, ReceiverField, null, null));
        return "receiver:borrow";
    }
}
