using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.CompilerServices;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal static class CSharpReferenceObjects
{
    private sealed record Plan(IReadOnlySet<string> Types);
    private static readonly ConditionalWeakTable<SemanticDocument, Plan> Plans = new();
    public static IReadOnlySet<string> Types(SemanticDocument document) => Plans.GetValue(document, Analyze).Types;
    public static string Payload(string type) => "type:$class:payload:" + type;
    public static string Guard(string type) => "function:$class:require:" + type;

    private static Plan Analyze(SemanticDocument document)
    {
        HashSet<string> candidates = document.ClassTypes.Where(type => type.IsSourceDeclared && !type.IsStatic
            && !type.IsAbstract && !type.IsGeneric && !type.IsRecord && !type.HasPrimaryConstructor
            && !type.HasInstanceInitializers && !type.HasStaticInitialization && !type.HasImplicitInstanceStorage
            && !type.HasVirtualMembers && !type.HasFinalizer && type.BaseTypeId == "type:object"
            && type.InterfaceTypeIds.Count == 0 && !document.UeTypeDeclarations.Any(ue => ue.TypeId == type.TypeId)
            && document.Symbols.Any(symbol => symbol.Kind == "type" && symbol.TypeId == type.TypeId))
            .Select(type => type.TypeId).ToHashSet(StringComparer.Ordinal);
        bool Reachable(string id) => document.Reachability is null || document.Reachability.ReachableCallableIds.Contains(id);
        HashSet<string> used = new(StringComparer.Ordinal);
        Stack<SemanticOperation> pending = new(document.Methods.Where(method => Reachable(method.MethodSymbolId)).Select(method => method.Root));
        foreach (SemanticBasicBlock block in document.ControlFlowGraphs.Where(graph => Reachable(graph.MethodSymbolId)).SelectMany(graph => graph.Blocks))
        {
            foreach (SemanticOperation operation in block.Operations) pending.Push(operation);
            if (block.BranchValue is not null) pending.Push(block.BranchValue);
        }
        while (pending.TryPop(out SemanticOperation? operation))
        {
            if (operation.TypeId is { } id) used.Add(id);
            foreach (SemanticOperation child in operation.Children) pending.Push(child);
        }
        foreach (SemanticCallable callable in document.Callables.Where(callable => Reachable(callable.MethodSymbolId)))
        {
            if (!callable.IsStatic) used.Add(callable.ContainingTypeId);
            used.Add(callable.ReturnTypeId);
            foreach (SemanticCallableParameter parameter in callable.Parameters) used.Add(parameter.TypeId);
        }
        // Value and closure layouts are emitted even for unused helper bodies.
        // Their reference fields still need a concrete type in the layout table.
        foreach (SemanticType type in document.Types.Where(type => type.Kind == "struct")) used.Add(type.Id);
        foreach (SemanticClosureCell cell in document.ClosureEnvironments.SelectMany(environment => environment.Cells)) used.Add(cell.TypeId);
        // Discover reference fields inside used value types as well as recursive object graphs.
        var owners = document.Symbols.Where(symbol => symbol.Kind == "type" && symbol.TypeId is not null)
            .ToDictionary(symbol => symbol.Id, symbol => symbol.TypeId!, StringComparer.Ordinal);
        bool changed;
        do { changed = false; foreach (SemanticSymbol field in document.Symbols.Where(symbol => symbol.Kind == "field" && !symbol.IsStatic))
            if (field.ContainingSymbolId is { } owner && owners.TryGetValue(owner, out string? type) && used.Contains(type)
                && field.TypeId is { } fieldType) changed |= used.Add(fieldType); } while (changed);
        candidates.IntersectWith(used);
        return new(candidates);
    }

    public static void AddTypes(SemanticDocument document, List<GuestType> types)
    {
        foreach (string id in Types(document).OrderBy(id => id, StringComparer.Ordinal))
        {
            string owner = document.Symbols.Single(symbol => symbol.Kind == "type" && symbol.TypeId == id).Id;
            GuestField[] fields = document.Symbols.Where(symbol => symbol.Kind == "field" && !symbol.IsStatic && symbol.ContainingSymbolId == owner)
                .OrderBy(symbol => symbol.Id, StringComparer.Ordinal).Select(symbol => new GuestField(symbol.Id, symbol.Name, symbol.TypeId!, 0)).ToArray();
            if (fields.Length == 0) fields = new[] { new GuestField("$empty:" + id, "$storage", "type:uint8", 0) };
            types.Add(new(Payload(id), "struct", "memory", fields, null, null, 0, 1));
            types.Add(new(id, "managed_ref", "i64", Array.Empty<GuestField>(), Payload(id), null, 8, 8));
        }
        if (Types(document).Count == 0) return;
        types.Add(new("type:object", "managed_ref", "i64", Array.Empty<GuestField>(), null, null, 8, 8));
        if (types.All(type => type.Id != "type:bool")) types.Add(new("type:bool", "scalar", "i32", Array.Empty<GuestField>(), null, null, 4, 4));
        if (types.All(type => type.Id != "type:void")) types.Add(new("type:void", "void", "none", Array.Empty<GuestField>(), null, null, 0, 1));
    }

    public static bool IsField(CSharpFunctionLoweringContext context, SemanticOperation operation) =>
        operation.Kind == "field_reference" && operation.Children.Count == 1
        && Types(context.Document).Contains(operation.Children[0].TypeId ?? "");

    public static void Require(GuestRegister value, List<GuestInstruction> instructions) =>
        instructions.Add(new("call", null, new[] { value.Id }, Guard(value.TypeId), null, null));

    public static void AddGuards(SemanticDocument document, List<GuestFunction> functions)
    {
        var methods = document.Callables.Where(callable => !callable.IsStatic && callable.HasBody && Types(document).Contains(callable.ContainingTypeId))
            .ToDictionary(callable => CSharpGuestIds.Function(callable.MethodSymbolId), callable => callable.ContainingTypeId, StringComparer.Ordinal);
        for (int i = 0; i < functions.Count; i++)
            if (methods.TryGetValue(functions[i].Id, out string? type))
                functions[i] = functions[i] with { Blocks = functions[i].Blocks.Select(block => block.Id == functions[i].EntryBlockId
                    ? block with { Instructions = new[] { new GuestInstruction("call", null, new[] { functions[i].Parameters[0].Id }, Guard(type), null, null) }
                        .Concat(block.Instructions).ToArray() } : block).ToArray() };
        foreach (string type in Types(document).OrderBy(id => id, StringComparer.Ordinal))
            functions.Add(new(Guard(type), new[] { new GuestRegister("receiver", type) },
                new[] { new GuestRegister("null", type), new GuestRegister("is_null", "type:bool") }, "type:void", "entry", new[] {
                    new GuestBasicBlock("entry", new[] { new GuestInstruction("constant", "null", Array.Empty<string>(), null, null, new("null", null)),
                        new GuestInstruction("binary", "is_null", new[] { "receiver", "null" }, null, "equals", null) }, new("branch_if", "is_null", "failed", "ok", null)),
                    new GuestBasicBlock("failed", Array.Empty<GuestInstruction>(), new("trap", null, null, null, null)),
                    new GuestBasicBlock("ok", Array.Empty<GuestInstruction>(), new("return", null, null, null, null)) }));
    }
}
