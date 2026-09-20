using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

// Roslyn captures an assignment location before branching through its RHS.
// Preserve that location, not its old value or an expression to evaluate again.
internal sealed class CSharpCapturedLocations
{
    private readonly CSharpFunctionLoweringContext context;
    private readonly HashSet<string> locationIds = new(StringComparer.Ordinal);
    private readonly HashSet<string> snapshotIds = new(StringComparer.Ordinal);
    private readonly Dictionary<string, GuestRegister> snapshots = new(StringComparer.Ordinal);
    private readonly Dictionary<string, (GuestRegister Storage, SemanticOperation Source, bool Borrowed)> locations = new(StringComparer.Ordinal);

    public CSharpCapturedLocations(CSharpFunctionLoweringContext context)
    {
        this.context = context;
        if (!CSharpBorrowedReferences.Enabled(context.Document)) return;
        var graph = context.Document.ControlFlowGraphs.SingleOrDefault(item => item.MethodSymbolId == context.Callable.MethodSymbolId);
        if (graph is null) return;
        var operations = graph.Blocks.SelectMany(block => block.Operations.Concat(
            block.BranchValue is null ? Array.Empty<SemanticOperation>() : new[] { block.BranchValue })).SelectMany(Walk).ToArray();
        foreach (var operation in operations)
        {
            if (operation.Kind is "assignment" or "compound_assignment" or "increment_or_decrement"
                && operation.Children.Count != 0) Mark(operation.Children[0]);
            if (operation.Kind == "compound_assignment" && operation.Children.Count != 0
                && operation.Children[0].Kind == "flow_capture_reference" && operation.Children[0].CaptureId is { } capture)
                snapshotIds.Add(capture);
            if (operation.Kind == "argument" && operation.Children.Count == 1
                && context.Document.Callables.SelectMany(callable => callable.Parameters)
                    .Any(parameter => parameter.SymbolId == operation.SymbolId && parameter.RefKind != "none"))
                Mark(operation.Children[0]);
        }
        // Ordinary value captures retain their snapshot semantics. Only field
        // captures used as locations need the descriptor below; scalar locals
        // and globals already have stable storage in the existing lowerer.
        locationIds.IntersectWith(operations.Where(operation => operation.Kind == "flow_capture"
            && operation.CaptureId is not null && operation.Children.Count == 1
            && CSharpBorrowedReferences.IsField(context, operation.Children[0])).Select(operation => operation.CaptureId!));
    }

    private void Mark(SemanticOperation operation)
    {
        if (operation.Kind == "flow_capture_reference" && operation.CaptureId is not null)
            locationIds.Add(operation.CaptureId);
        else if (operation.Kind is "argument" or "declaration_expression" && operation.Children.Count == 1)
            Mark(operation.Children[0]);
        else if (operation.Kind == "field_reference" && operation.Children.Count == 1
            && context.TryGetGuestType(operation.Children[0].TypeId, out var type) && type.Kind == "struct")
            Mark(operation.Children[0]);
    }

    private static IEnumerable<SemanticOperation> Walk(SemanticOperation operation)
    {
        yield return operation;
        foreach (var child in operation.Children)
            foreach (var nested in Walk(child)) yield return nested;
    }

    public bool Contains(string? id) => id is not null && locationIds.Contains(id);

    public GuestRegister? Capture(SemanticOperation operation, int block, List<GuestInstruction> instructions)
    {
        var source = operation.Children[0];
        if (!CSharpBorrowedReferences.IsField(context, source))
        {
            context.Add("ASCG1024", "A captured field location cannot merge with an ordinary value.");
            return null;
        }
        bool borrowed = !CSharpReferenceObjects.IsField(context, source);
        var value = borrowed ? CSharpBorrowedReferences.Address(context, source, block, instructions)
            : CSharpOperationLowerer.LowerValue(context, source.Children[0], block, instructions);
        if (value is null) return null;
        if (!locations.TryGetValue(operation.CaptureId!, out var location))
        {
            var storage = context.CreateTemporary(value.TypeId, block);
            if (storage is null) return null;
            location = (storage, source, borrowed);
            locations.Add(operation.CaptureId!, location);
        }
        if (location.Storage.TypeId != value.TypeId || location.Borrowed != borrowed
            || location.Source.TypeId != source.TypeId || (!borrowed && location.Source.SymbolId != source.SymbolId))
        {
            context.Add("ASCG1024", "A captured field location changed its storage contract.");
            return null;
        }
        instructions.Add(new("local_store", null, new[] { value.Id }, location.Storage.Id, null, null));
        if (snapshotIds.Contains(operation.CaptureId!))
        {
            // Compound assignment reads its old left value before the branching
            // RHS, even if that RHS writes through an alias of this location.
            if (!snapshots.TryGetValue(operation.CaptureId!, out var snapshot))
            {
                snapshot = context.CreateTemporary(source.TypeId, block);
                if (snapshot is null) return null;
                snapshots.Add(operation.CaptureId!, snapshot);
            }
            instructions.Add(new(borrowed ? "borrow_load" : "managed_get", snapshot.Id,
                new[] { location.Storage.Id }, borrowed ? source.TypeId : source.SymbolId, null, null));
        }
        return value;
    }

    public GuestRegister? ReadSnapshot(string? id)
    {
        if (id is not null && snapshots.TryGetValue(id, out var value)) return value;
        context.Add("ASCG1024", "Captured compound assignment has no pre-RHS value snapshot.");
        return null;
    }

    public GuestRegister? Address(string? id, int block, List<GuestInstruction> instructions)
    {
        if (!TryGet(id, out var location)) return null;
        if (location.Borrowed) return location.Storage;
        CSharpReferenceObjects.Require(location.Storage, instructions);
        var reference = context.CreateTemporary(CSharpBorrowedReferences.Type(location.Source.TypeId!), block);
        if (reference is not null) instructions.Add(new("borrow_managed", reference.Id,
            new[] { location.Storage.Id }, location.Source.SymbolId, null, null));
        return reference;
    }

    public GuestRegister? Read(string? id, string? type, int block, List<GuestInstruction> instructions)
    {
        if (!TryGet(id, out var location)) return null;
        var value = context.CreateTemporary(type, block);
        if (value is not null) instructions.Add(new(location.Borrowed ? "borrow_load" : "managed_get", value.Id,
            new[] { location.Storage.Id }, location.Borrowed ? type : location.Source.SymbolId, null, null));
        return value;
    }

    public bool Store(string? id, GuestRegister value, List<GuestInstruction> instructions)
    {
        if (!TryGet(id, out var location)) return false;
        instructions.Add(new(location.Borrowed ? "borrow_store" : "managed_set", null,
            new[] { location.Storage.Id, value.Id }, location.Borrowed ? value.TypeId : location.Source.SymbolId, null, null));
        return true;
    }

    private bool TryGet(string? id, out (GuestRegister Storage, SemanticOperation Source, bool Borrowed) location)
    {
        if (id is not null && locations.TryGetValue(id, out location)) return true;
        context.Add("ASCG1024", "Captured field location was used before its storage was lowered.");
        location = default;
        return false;
    }
}
