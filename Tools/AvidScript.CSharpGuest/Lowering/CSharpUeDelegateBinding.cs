using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal sealed record CSharpUeDelegatePlan(string Id, SemanticDelegateType Signature,
    SemanticOperation Reference, IReadOnlyList<CSharpUeDispatchTarget> Targets);

internal static class CSharpUeDelegateBinding
{
    public const string ImportId = "import:$ue:receiver:type";
    public const string ImportName = "avid_ue_receiver_type_v1";

    public static bool IsUeReference(SemanticDocument document, SemanticOperation operation) =>
        operation is { Kind: "method_reference", Children.Count: 1, SymbolId: { } id }
        && document.UeMethodCatalog?.Methods.Any(method => method.MethodSymbolId == id) == true;

    public static CSharpUeDelegatePlan? Plan(SemanticDocument document, SemanticOperation creation)
    {
        if (creation.Kind != "delegate_creation" || creation.Children.Count != 1
            || !IsUeReference(document, creation.Children[0])) return null;
        SemanticOperation reference = creation.Children[0];
        var signature = document.DelegateTypes.SingleOrDefault(item => item.TypeId == creation.TypeId);
        var declaration = document.UeMethodCatalog!.Methods.Single(method => method.MethodSymbolId == reference.SymbolId);
        if (signature is null || signature.ReturnRefKind != "none" || declaration.ReturnRefKind != "none"
            || declaration.ReturnTypeId != signature.ReturnTypeId
            || !declaration.Parameters.Select(parameter => (parameter.TypeId, parameter.RefKind))
                .SequenceEqual(signature.Parameters.Select(parameter => (parameter.TypeId, parameter.RefKind)))) return null;
        IReadOnlyList<CSharpUeDispatchTarget> targets;
        if (reference.Dispatch?.Kind is "virtual" or "interface")
        {
            if (!CSharpUeDispatch.TryRoute(document, reference, out var route)) return null;
            targets = route.Targets;
        }
        else
        {
            targets = document.UeTypeDeclarations.Select((type, ordinal) => (type, ordinal))
                .Where(item => IsDerived(document, item.type.TypeId, declaration.ContainingTypeId))
                .Select(item => new CSharpUeDispatchTarget((uint)item.ordinal, declaration)).ToArray();
        }
        if (targets.Count == 0 || targets.Any(target => !target.Method.HasGuestBody
            || !CSharpUeReceivers.IsType(document, target.Method.ContainingTypeId))) return null;
        return new("function:$ue:bind:" + reference.Dispatch?.Kind + ":" + declaration.MethodSymbolId + ":" + signature.TypeId,
            signature, reference, targets);
    }

    public static IReadOnlyList<CSharpUeDelegatePlan> Plans(SemanticDocument document) =>
        document.ControlFlowGraphs.Where(graph => document.Reachability is null
            || document.Reachability.ReachableCallableIds.Contains(graph.MethodSymbolId))
        .SelectMany(graph => graph.Blocks.Where(block => block.IsReachable))
        .SelectMany(block => block.BranchValue is null ? block.Operations : block.Operations.Append(block.BranchValue))
        .SelectMany(Operations).Select(operation => Plan(document, operation)).Where(plan => plan is not null)
        .Cast<CSharpUeDelegatePlan>().DistinctBy(plan => plan.Id).OrderBy(plan => plan.Id, StringComparer.Ordinal).ToArray();

    public static GuestRegister? Create(CSharpFunctionLoweringContext context, SemanticOperation operation,
        int block, List<GuestInstruction> instructions)
    {
        var plan = Plan(context.Document, operation);
        if (plan is null) { context.Add("ASCG1024", "UE method group requires a matching delegate signature and registered synchronous implementations."); return null; }
        var receiver = CSharpOperationLowerer.LowerValue(context, plan.Reference.Children[0], block, instructions);
        if (receiver is null) return null;
        var packed = context.CreateTemporary("type:uint64", block)!;
        var result = context.CreateTemporary(plan.Signature.TypeId, block)!;
        instructions.Add(new("convert", packed.Id, new[] { receiver.Id }, null, null, null));
        instructions.Add(new("call", result.Id, new[] { packed.Id }, plan.Id, null, null));
        return result;
    }

    public static IReadOnlyList<GuestFunction> Build(SemanticDocument document) => Plans(document).Select(BuildOne).ToArray();

    private static GuestFunction BuildOne(CSharpUeDelegatePlan plan)
    {
        List<GuestRegister> locals = new() { new("ordinal", "type:int32") };
        List<GuestBasicBlock> blocks = new() { new("entry",
            new[] { new GuestInstruction("call", "ordinal", new[] { "receiver" }, ImportId, null, null) },
            new("branch", null, "case:0", null, null)) };
        for (int i = 0; i < plan.Targets.Count; ++i)
        {
            var target = plan.Targets[i]; string prefix = "case:" + i;
            string Local(string name, string type) { string id = prefix + ":" + name; locals.Add(new(id, type)); return id; }
            string key = Local("key", "type:int32"), match = Local("match", "type:bool");
            blocks.Add(new(prefix, new[] {
                new GuestInstruction("constant", key, Array.Empty<string>(), null, null, new("int32", (target.TypeOrdinal + 1).ToString(System.Globalization.CultureInfo.InvariantCulture))),
                new GuestInstruction("binary", match, new[] { "ordinal", key }, null, "equals", null) },
                new("branch_if", match, prefix + ":bind", i + 1 < plan.Targets.Count ? "case:" + (i + 1) : "invalid", null)));
            string receiver = Local("receiver", target.Method.ContainingTypeId);
            string box = Local("box", CSharpClosureLayout.Reference(CSharpBoundDelegateLowerer.Box(target.Method.MethodSymbolId)));
            string erased = Local("context", CSharpClosureLayout.ObjectType);
            string function = Local("function", CSharpClosureLayout.FunctionType(plan.Signature.TypeId));
            string result = Local("delegate", plan.Signature.TypeId);
            blocks.Add(new(prefix + ":bind", new[] {
                new GuestInstruction("convert", receiver, new[] { "receiver" }, null, null, null),
                new GuestInstruction("managed_new", box, Array.Empty<string>(), null, null, null),
                new GuestInstruction("managed_set", null, new[] { box, receiver }, CSharpBoundDelegateLowerer.ReceiverField, null, null),
                new GuestInstruction("managed_cast", erased, new[] { box }, null, null, null),
                new GuestInstruction("function_ref", function, Array.Empty<string>(), CSharpClosureLayout.Thunk(target.Method.MethodSymbolId, plan.Signature.TypeId), null, null),
                new GuestInstruction("constant", result, Array.Empty<string>(), null, null, new("zero", null)),
                new GuestInstruction("field_store", null, new[] { result, function }, CSharpClosureLayout.TargetField, null, null),
                new GuestInstruction("field_store", null, new[] { result, erased }, CSharpClosureLayout.ContextField, null, null) },
                new("return", null, null, null, result)));
        }
        blocks.Add(new("invalid", Array.Empty<GuestInstruction>(), new("trap", null, null, null, null)));
        return new(plan.Id, new[] { new GuestRegister("receiver", "type:uint64") }, locals, plan.Signature.TypeId, "entry", blocks);
    }

    private static bool IsDerived(SemanticDocument document, string type, string ancestor)
    {
        HashSet<string> visited = new(StringComparer.Ordinal);
        while (visited.Add(type))
        {
            if (type == ancestor) return true;
            string? parent = document.ClassTypes.SingleOrDefault(item => item.TypeId == type)?.BaseTypeId;
            if (parent is null) return false;
            type = parent;
        }
        return false;
    }

    private static IEnumerable<SemanticOperation> Operations(SemanticOperation operation)
    {
        yield return operation;
        foreach (var child in operation.Children.SelectMany(Operations)) yield return child;
    }
}
