using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal sealed class CSharpClosureCells
{
    private readonly CSharpFunctionLoweringContext context;
    private readonly Dictionary<string, (GuestRegister Environment, string Field)> cells = new(StringComparer.Ordinal);
    private readonly Dictionary<string, GuestRegister> environments = new(StringComparer.Ordinal);
    public CSharpClosureCells(CSharpFunctionLoweringContext context)
    {
        this.context = context;
        foreach (SemanticClosureEnvironment environment in context.Document.ClosureEnvironments.Where(item => item.OwnerMethodSymbolId == context.Callable.MethodSymbolId))
        {
            GuestRegister register = context.CreateTemporary(CSharpClosureLayout.Reference(environment.Id), -1)!;
            environments.Add(environment.Id, register);
            foreach (SemanticClosureCell cell in environment.Cells) cells.Add(cell.SymbolId, (register, cell.SymbolId));
        }
        foreach (SemanticCallableParameter parameter in context.Callable.Parameters)
            if (CSharpClosureLayout.CapturedParameter(context.Document, parameter.SymbolId) is { } captured
                && context.TryGetStorage(parameter.SymbolId, out GuestRegister register))
            {
                cells.Add(parameter.SymbolId, (register, captured.Cell.SymbolId));
                environments.TryAdd(captured.Environment.Id, register);
            }
    }
    public bool Has(string? symbol) => symbol is not null && cells.ContainsKey(symbol);
    public bool RequiresManagedAddress(SemanticOperation operation) => RequiresManagedAddress(operation, new(StringComparer.Ordinal));
    private bool RequiresManagedAddress(SemanticOperation operation, HashSet<string> visited)
    {
        if (operation.Kind is "local_reference" or "parameter_reference") return Has(operation.SymbolId);
        if (operation.Kind == "flow_capture_reference" && operation.CaptureId is { } capture
            && visited.Add(capture) && context.TryGetCaptureTarget(capture, out SemanticOperation source))
            return RequiresManagedAddress(source, visited);
        return operation.Kind == "field_reference" && operation.Children.Count == 1
            && context.TryGetGuestType(operation.Children[0].TypeId, out GuestType parent) && parent.Kind == "struct"
            && RequiresManagedAddress(operation.Children[0], visited);
    }
    public bool RejectValueReceiverBorrow(SemanticOperation operation)
    {
        if (!context.TryGetGuestType(operation.TypeId, out GuestType type) || type.Kind != "struct"
            || !RequiresManagedAddress(operation)) return false;
        context.Add("ASCG1024", "A captured value receiver requires a managed interior reference adapter; mutating a detached copy would lose shared writes.");
        return true;
    }
    public GuestRegister? Environment(string id) => environments.GetValueOrDefault(id);
    public bool TryLoad(string? symbol, string? type, int block, List<GuestInstruction> instructions, out GuestRegister? value)
    {
        value = null;
        if (symbol is null || !cells.TryGetValue(symbol, out var cell)) return false;
        value = context.CreateTemporary(type, block);
        if (value is not null) instructions.Add(new("managed_get", value.Id, new[] { cell.Environment.Id }, cell.Field, null, null));
        return true;
    }
    public bool TryStore(string? symbol, GuestRegister value, List<GuestInstruction> instructions)
    {
        if (symbol is null || !cells.TryGetValue(symbol, out var cell)) return false;
        instructions.Add(new("managed_set", null, new[] { cell.Environment.Id, value.Id }, cell.Field, null, null));
        return true;
    }

    public bool InsertAllocations(SemanticControlFlowGraph graph, List<GuestBasicBlock> blocks)
    {
        SemanticClosureEnvironment[] owned = context.Document.ClosureEnvironments.Where(item => item.OwnerMethodSymbolId == context.Callable.MethodSymbolId).ToArray();
        if (owned.Length == 0) return true;
        List<GuestInstruction> entry = new();
        foreach (SemanticClosureEnvironment environment in owned.Where(item => item.ScopeKind == "activation")) Allocate(environment, entry);
        int index = blocks.FindIndex(block => block.Id == CSharpGuestIds.Block(graph.MethodSymbolId, graph.EntryBlockOrdinal));
        blocks[index] = blocks[index] with { Instructions = entry.Concat(blocks[index].Instructions).ToArray() };
        int edgeOrdinal = 0;
        foreach (SemanticControlFlowEdge edge in graph.Blocks.SelectMany(block => block.Successors)
                     .Where(edge => edge.Semantics == "regular").DistinctBy(edge => (edge.SourceBlockOrdinal, edge.DestinationBlockOrdinal)))
        {
            List<GuestInstruction> instructions = new();
            foreach (SemanticClosureEnvironment environment in owned.Where(item => item.ScopeKind != "activation"))
            {
                SemanticClosureAllocation plan = environment.Allocation!;
                bool insideSource = edge.SourceBlockOrdinal >= plan.FirstBlockOrdinal && edge.SourceBlockOrdinal <= plan.LastBlockOrdinal;
                bool insideTarget = edge.DestinationBlockOrdinal >= plan.FirstBlockOrdinal && edge.DestinationBlockOrdinal <= plan.LastBlockOrdinal;
                if (insideSource && !insideTarget)
                {
                    GuestRegister empty = context.CreateTemporary(CSharpClosureLayout.Reference(environment.Id), -1)!;
                    instructions.Add(new("constant", empty.Id, Array.Empty<string>(), null, null, new("null", null)));
                    instructions.Add(new("local_store", null, new[] { empty.Id }, environments[environment.Id].Id, null, null));
                }
            }
            foreach (SemanticClosureEnvironment environment in owned.Where(item => item.Allocation!.Entries.Contains(new(edge.SourceBlockOrdinal, edge.DestinationBlockOrdinal)))
                         .OrderBy(item => item.Allocation!.FirstBlockOrdinal).ThenByDescending(item => item.Allocation!.LastBlockOrdinal)) Allocate(environment, instructions);
            if (instructions.Count == 0) continue;
            string source = CSharpGuestIds.Block(graph.MethodSymbolId, edge.SourceBlockOrdinal), target = CSharpGuestIds.Block(graph.MethodSymbolId, edge.DestinationBlockOrdinal);
            // Short-circuit lowering may have split the original source block. Match
            // the final branch edge rather than assuming that block still owns it.
            int sourceIndex = blocks.FindIndex(block => (block.Id == source || block.Id.StartsWith(source + ":", StringComparison.Ordinal))
                && (block.Terminator.TargetBlockId == target || block.Terminator.FalseTargetBlockId == target));
            if (sourceIndex < 0) { context.Add("ASCG1024", "Closure allocation edge was lost during CFG lowering."); return false; }
            string bridge = source + ":$closure:edge:" + edgeOrdinal++;
            GuestTerminator previous = blocks[sourceIndex].Terminator;
            blocks[sourceIndex] = blocks[sourceIndex] with { Terminator = previous with
            { TargetBlockId = previous.TargetBlockId == target ? bridge : previous.TargetBlockId,
                FalseTargetBlockId = previous.FalseTargetBlockId == target ? bridge : previous.FalseTargetBlockId } };
            blocks.Add(new(bridge, instructions, new("branch", null, target, null, null)));
        }
        return true;

        void Allocate(SemanticClosureEnvironment environment, List<GuestInstruction> instructions)
        {
            GuestRegister allocated = context.CreateTemporary(CSharpClosureLayout.Reference(environment.Id), -1)!;
            instructions.Add(new("managed_new", allocated.Id, Array.Empty<string>(), null, null, null));
            instructions.Add(new("local_store", null, new[] { allocated.Id }, environments[environment.Id].Id, null, null));
            foreach (SemanticClosureCell cell in environment.Cells.Where(cell => cell.Kind == "parameter"))
                if (context.TryGetStorage(cell.SymbolId, out GuestRegister parameter))
                    instructions.Add(new("managed_set", null, new[] { allocated.Id, parameter.Id }, cell.SymbolId, null, null));
        }
    }
}
