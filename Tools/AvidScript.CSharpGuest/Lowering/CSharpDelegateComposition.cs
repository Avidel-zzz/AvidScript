using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.CompilerServices;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal static class CSharpDelegateComposition
{
    private sealed record Plan(IReadOnlySet<string> Signatures);
    private static readonly ConditionalWeakTable<SemanticDocument, Plan> Plans = new();
    public static IReadOnlySet<string> Signatures(SemanticDocument document) => Plans.GetValue(document, Analyze).Signatures;
    public static string Function(string signature, string operation) => "function:$delegate:list:" + operation + ":" + signature;
    public static string Node(string signature) => "$delegate:list:" + signature;
    public const string Left = "$left", Right = "$right", LeftCount = "$left_count", Count = "$count";

    private static Plan Analyze(SemanticDocument document)
    {
        HashSet<string> delegates = document.DelegateTypes.Select(type => type.TypeId).ToHashSet(StringComparer.Ordinal);
        HashSet<string> used = new(StringComparer.Ordinal);
        Stack<SemanticOperation> pending = new(document.Methods.Select(method => method.Root));
        foreach (SemanticBasicBlock block in document.ControlFlowGraphs.SelectMany(graph => graph.Blocks))
        {
            foreach (SemanticOperation operation in block.Operations) pending.Push(operation);
            if (block.BranchValue is not null) pending.Push(block.BranchValue);
        }
        while (pending.TryPop(out SemanticOperation? operation))
        {
            if (operation.Kind is "binary" or "compound_assignment" && operation.OperatorKind is "add" or "subtract"
                && operation.TypeId is { } type && delegates.Contains(type)) used.Add(type);
            foreach (SemanticOperation child in operation.Children) pending.Push(child);
        }
        return new(used);
    }

    public static void AddTypes(SemanticDocument document, List<GuestType> types)
    {
        if (Signatures(document).Count != 0)
        {
            if (!types.Any(type => type.Id == CSharpGuestIds.Int32TypeId))
                types.Add(new(CSharpGuestIds.Int32TypeId, "scalar", "i32", Array.Empty<GuestField>(), null, null, 4, 4));
            if (!types.Any(type => type.Id == "type:bool"))
                types.Add(new("type:bool", "scalar", "i32", Array.Empty<GuestField>(), null, null, 1, 1));
        }
        foreach (string signature in Signatures(document).OrderBy(value => value, StringComparer.Ordinal))
        {
            string payload = CSharpClosureLayout.Payload(Node(signature));
            types.Add(new(payload, "struct", "memory", new[] {
                new GuestField(Left, Left, signature, 0), new GuestField(Right, Right, signature, 0),
                new GuestField(LeftCount, LeftCount, CSharpGuestIds.Int32TypeId, 0),
                new GuestField(Count, Count, CSharpGuestIds.Int32TypeId, 0) }, null, null, 0, 1));
            types.Add(new(CSharpClosureLayout.Reference(Node(signature)), "managed_ref", "i64", Array.Empty<GuestField>(), payload, null, 8, 8));
        }
    }

    public static GuestRegister? Lower(CSharpFunctionLoweringContext context, SemanticOperation operation, int block, List<GuestInstruction> instructions)
    {
        if (operation.Children.Count != 2) return null;
        GuestRegister? left = CSharpOperationLowerer.LowerValue(context, operation.Children[0], block, instructions);
        GuestRegister? right = CSharpOperationLowerer.LowerValue(context, operation.Children[1], block, instructions);
        GuestRegister? result = context.CreateTemporary(operation.TypeId, block);
        return left is not null && right is not null && result is not null
            && Emit(context, operation.OperatorKind, left, right, result, instructions) ? result : null;
    }

    public static bool Emit(CSharpFunctionLoweringContext context, string? operation, GuestRegister left, GuestRegister right,
        GuestRegister result, List<GuestInstruction> instructions)
    {
        if (operation is not ("add" or "subtract") || left.TypeId != right.TypeId || result.TypeId != left.TypeId
            || !Signatures(context.Document).Contains(result.TypeId))
        { context.Add("ASCG1024", "Delegate composition requires matching nominal signatures and a supported list operation."); return false; }
        instructions.Add(new("call", result.Id, new[] { left.Id, right.Id }, Function(result.TypeId, operation), null, null));
        return true;
    }

    public static IReadOnlyList<GuestFunction> Build(SemanticDocument document) => document.DelegateTypes
        .Where(signature => Signatures(document).Contains(signature.TypeId)).OrderBy(signature => signature.TypeId, StringComparer.Ordinal)
        .SelectMany(signature => CSharpDelegateListFunctions.Build(document, signature)).ToArray();
}
