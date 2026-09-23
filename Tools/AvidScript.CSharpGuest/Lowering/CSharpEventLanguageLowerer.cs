using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Runtime.CompilerServices;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

// Compiler-owned bridge for generated C# event add/remove accessors. The generated
// accessor bodies are compile-time facades; only these helpers perform Host effects.
internal static class CSharpEventLanguageLowerer
{
    private sealed record Plan(IReadOnlySet<string> EventSymbolIds);
    private static readonly ConditionalWeakTable<SemanticDocument, Plan> Plans = new();
    public static string Function(SemanticEventSubscription entry, string operation) =>
        "function:$event:language:" + entry.SubscriptionId + ":" + operation;

    public static IReadOnlySet<string> Used(SemanticDocument document) =>
        Plans.GetValue(document, Analyze).EventSymbolIds;

    private static Plan Analyze(SemanticDocument document)
    {
        HashSet<string> used = new(StringComparer.Ordinal);
        foreach (SemanticOperation root in document.Methods.Select(method => method.Root)
            .Concat(document.ControlFlowGraphs.SelectMany(graph => graph.Blocks)
                .SelectMany(block => block.Operations.Concat(block.BranchValue is null
                    ? Array.Empty<SemanticOperation>() : new[] { block.BranchValue })))
            .Concat(document.AsyncMethods.SelectMany(method => method.Segments)
                .SelectMany(segment => segment.Statements.Select(statement => statement.Operation))))
        {
            foreach (SemanticOperation operation in Flatten(root))
                if (operation.Kind == "event_assignment" && operation.SymbolId is not null)
                    used.Add(operation.SymbolId);
        }
        return new(used);
    }

    public static GuestRegister? Lower(CSharpFunctionLoweringContext context, SemanticOperation operation,
        int block, List<GuestInstruction> instructions)
    {
        SemanticEventSubscription? entry = context.Document.EventSubscriptions.SingleOrDefault(item =>
            item.EventSymbolId is not null && item.EventSymbolId == operation.SymbolId);
        if (entry?.OwnerTypeId is null || operation.OperatorKind is not ("add" or "remove")
            || operation.Children.Count != 2 || operation.Children[0] is not { Kind: "event_reference", Children.Count: 1 } reference
            || reference.SymbolId != entry.EventSymbolId || reference.TypeId != entry.DelegateTypeId
            || operation.Children[1].TypeId != entry.DelegateTypeId
            || reference.Children[0].TypeId != entry.OwnerTypeId
            || !context.TryGetGuestType(entry.OwnerTypeId, out GuestType ownerType) || ownerType.Kind != "struct")
        {
            context.Add("ASCG1024", "Language event assignment requires a validated generated owner, receiver and exact delegate type.");
            return null;
        }

        string ownerSymbol = "symbol:" + entry.OwnerTypeId;
        SemanticSymbol? slotField = Field("Slot"), generationField = Field("Generation");
        if (slotField is null || generationField is null)
        {
            context.Add("ASCG1024", "Language event owner is missing its generated handle fields.");
            return null;
        }
        GuestRegister? receiver = CSharpOperationLowerer.LowerValue(context, reference.Children[0], block, instructions);
        GuestRegister? handler = receiver is null ? null : CSharpOperationLowerer.LowerValue(context, operation.Children[1], block, instructions);
        if (receiver is null || handler is null) return null;
        GuestRegister? slot = context.CreateTemporary("type:int32", block);
        GuestRegister? generation = context.CreateTemporary("type:int32", block);
        if (slot is null || generation is null) return null;
        instructions.Add(new("field_load", slot.Id, new[] { receiver.Id }, slotField.Id, null, null));
        instructions.Add(new("field_load", generation.Id, new[] { receiver.Id }, generationField.Id, null, null));
        instructions.Add(new("call", null, new[] { slot.Id, generation.Id, handler.Id },
            Function(entry, operation.OperatorKind), null, null));
        return null;

        SemanticSymbol? Field(string name) => context.Document.Symbols.SingleOrDefault(symbol =>
            symbol.ContainingSymbolId == ownerSymbol && symbol.Kind == "field"
            && symbol.Name == name && symbol.TypeId == "type:int32"
            && !symbol.IsStatic && symbol.IsReadonly && symbol.IsExecutableReferenceSource);
    }

    public static IReadOnlyList<GuestFunction> Build(SemanticDocument document) => document.EventSubscriptions
        .Where(entry => entry.EventSymbolId is not null && Used(document).Contains(entry.EventSymbolId))
        .OrderBy(entry => entry.SubscriptionId, StringComparer.Ordinal)
        .SelectMany(entry => new[] { BuildAdd(entry), BuildRemove(entry) }).ToArray();

    private static GuestFunction BuildAdd(SemanticEventSubscription entry)
    {
        Builder b = new(entry, "add");
        string box = b.Lookup(), target = b.Target("handler");
        b.Branch(b.Binary("equals", target, b.Nil(CSharpClosureLayout.FunctionType(entry.DelegateTypeId))), "done", "present");
        b.At("present");
        b.Branch(b.Binary("equals", box, b.Nil(b.BoxType)), "create", "combine");
        b.At("create");
        string created = b.Value(b.BoxType, "managed_new");
        b.Set(created, CSharpEventSubscriptions.HandlerField, "handler");
        string token = b.Value("type:int64", GuestEventState.LanguageSubscribeOp,
            new[] { "slot", "generation", b.Ordinal, created }, CSharpEventSubscriptions.LanguageSubscribeImport);
        b.Branch(b.Binary("equals", token, b.Value("type:int64", "constant", constant: new("int64", "0"))),
            "failed", "installed");
        b.At("failed"); b.Trap();
        b.At("installed"); b.Set(created, CSharpEventSubscriptions.TokenField, token); b.Return();
        b.At("combine");
        string old = b.Field(box, CSharpEventSubscriptions.HandlerField, entry.DelegateTypeId);
        string combined = b.Value(entry.DelegateTypeId, "call", new[] { old, "handler" },
            CSharpDelegateComposition.Function(entry.DelegateTypeId, "add"));
        b.Set(box, CSharpEventSubscriptions.HandlerField, combined); b.Return();
        b.At("done"); b.Return();
        return b.Finish();
    }

    private static GuestFunction BuildRemove(SemanticEventSubscription entry)
    {
        Builder b = new(entry, "remove");
        string box = b.Lookup(), target = b.Target("handler");
        b.Branch(b.Binary("equals", target, b.Nil(CSharpClosureLayout.FunctionType(entry.DelegateTypeId))), "done", "present");
        b.At("present");
        b.Branch(b.Binary("equals", box, b.Nil(b.BoxType)), "done", "subtract");
        b.At("subtract");
        string old = b.Field(box, CSharpEventSubscriptions.HandlerField, entry.DelegateTypeId);
        string updated = b.Value(entry.DelegateTypeId, "call", new[] { old, "handler" },
            CSharpDelegateComposition.Function(entry.DelegateTypeId, "subtract"));
        b.Branch(b.Binary("equals", b.Target(updated),
            b.Nil(CSharpClosureLayout.FunctionType(entry.DelegateTypeId))), "cancel", "store");
        b.At("store"); b.Set(box, CSharpEventSubscriptions.HandlerField, updated); b.Return();
        b.At("cancel");
        b.Set(box, CSharpEventSubscriptions.HandlerField, updated);
        string token = b.Field(box, CSharpEventSubscriptions.TokenField, "type:int64");
        b.Value("type:int32", "call", new[] { token }, CSharpEventSubscriptions.CancelImport);
        b.Return();
        b.At("done"); b.Return();
        return b.Finish();
    }

    private static IEnumerable<SemanticOperation> Flatten(SemanticOperation operation)
    {
        yield return operation;
        foreach (SemanticOperation child in operation.Children)
            foreach (SemanticOperation nested in Flatten(child)) yield return nested;
    }

    private sealed class Builder
    {
        private readonly SemanticEventSubscription entry;
        private readonly string operation;
        private readonly List<GuestRegister> locals = new();
        private readonly List<GuestBasicBlock> blocks = new();
        private List<GuestInstruction> instructions = new();
        private string block = "entry";
        public string BoxType => CSharpClosureLayout.Reference(CSharpEventSubscriptions.Box(entry));
        public string Ordinal { get; }

        public Builder(SemanticEventSubscription entry, string operation)
        {
            this.entry = entry;
            this.operation = operation;
            Ordinal = Value("type:int32", "constant", constant:
                new("int32", entry.EventOrdinal.ToString(CultureInfo.InvariantCulture)));
        }

        public string Value(string type, string op, string[]? sources = null, string? target = null,
            GuestConstant? constant = null, string? operatorKind = null)
        {
            string id = "v" + locals.Count;
            locals.Add(new(id, type));
            instructions.Add(new(op, id, sources ?? Array.Empty<string>(), target, operatorKind, constant));
            return id;
        }
        public string Nil(string type) => Value(type, "constant", constant: new("null", null));
        public string Binary(string operation, string left, string right) =>
            Value("type:bool", "binary", new[] { left, right }, operatorKind: operation);
        public string Field(string owner, string field, string type) => Value(type, "managed_get", new[] { owner }, field);
        public string Target(string handler) => Value(CSharpClosureLayout.FunctionType(entry.DelegateTypeId),
            "field_load", new[] { handler }, CSharpClosureLayout.TargetField);
        public void Set(string owner, string field, string value) =>
            instructions.Add(new("managed_set", null, new[] { owner, value }, field, null, null));
        public string Lookup() => Value(BoxType, GuestEventState.LanguageLookupOp,
            new[] { "slot", "generation", Ordinal }, CSharpEventSubscriptions.LanguageLookupImport);
        public void Branch(string condition, string yes, string no) => End(new("branch_if", condition, yes, no, null));
        public void Return() => End(new("return", null, null, null, null));
        public void Trap() => End(new("trap", null, null, null, null));
        private void End(GuestTerminator terminator)
        {
            blocks.Add(new(block, instructions.ToArray(), terminator));
            instructions = new();
        }
        public void At(string name)
        {
            if (instructions.Count != 0) throw new InvalidOperationException("Unterminated event helper block.");
            block = name;
        }
        public GuestFunction Finish() => new(Function(entry, operation),
            new[] { new GuestRegister("slot", "type:int32"), new GuestRegister("generation", "type:int32"),
                new GuestRegister("handler", entry.DelegateTypeId) },
            locals, "type:void", "entry", blocks);
    }
}
