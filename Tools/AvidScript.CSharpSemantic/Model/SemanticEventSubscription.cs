using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json.Serialization;

namespace AvidScript.CSharpSemantic;

// A generated, typed compiler boundary. No Guest heap identity appears in C#.
public sealed record SemanticEventSubscription(
    [property: JsonPropertyOrder(0), JsonRequired] string SubscriptionId,
    [property: JsonPropertyOrder(1), JsonRequired] int EventOrdinal,
    [property: JsonPropertyOrder(2), JsonRequired] string MethodSymbolId,
    [property: JsonPropertyOrder(3), JsonRequired] string DelegateTypeId,
    [property: JsonPropertyOrder(4), JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)] string? EventSymbolId = null,
    [property: JsonPropertyOrder(5), JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)] string? OwnerTypeId = null)
{
    [JsonIgnore]
    public string ExportName => "avid_on_delegate_" + SubscriptionId[..16] + "_state_v1";
}

public static class SemanticEventSubscriptionValidator
{
    public static bool IsValid(SemanticDocument document)
    {
        if (document.EventSubscriptions is null) return false;
        if (document.SchemaVersion < 29) return document.EventSubscriptions.Count == 0;
        HashSet<string> methods = new(StringComparer.Ordinal), events = new(StringComparer.Ordinal), exports = new(StringComparer.Ordinal);
        HashSet<string> languageSymbols = new(StringComparer.Ordinal);
        HashSet<int> ordinals = new();
        string? previous = null;
        foreach (SemanticEventSubscription entry in document.EventSubscriptions)
        {
            if (entry is null || entry.SubscriptionId is not { Length: 64 } id
                || id.Any(c => c is not (>= '0' and <= '9' or >= 'a' and <= 'f'))
                || entry.EventOrdinal < 0 || entry.EventOrdinal == int.MaxValue || !ordinals.Add(entry.EventOrdinal) || !events.Add(id)
                || !exports.Add(entry.ExportName) || string.IsNullOrWhiteSpace(entry.MethodSymbolId)
                || !methods.Add(entry.MethodSymbolId) || string.IsNullOrWhiteSpace(entry.DelegateTypeId)
                || previous is not null && StringComparer.Ordinal.Compare(previous, id) >= 0) return false;
            previous = id;
            SemanticCallable? callable = document.Callables.SingleOrDefault(item => item.MethodSymbolId == entry.MethodSymbolId);
            SemanticDelegateType? signature = document.DelegateTypes.SingleOrDefault(item => item.TypeId == entry.DelegateTypeId);
            if (callable is null || !callable.IsStatic || callable.IsConstructor || callable.HasBody
                || callable.Import is not null || callable.Export is not null || callable.Optimization is not null
                || callable.ReturnTypeId != "type:int64" || callable.Parameters.Count != 3
                || !callable.Parameters.Select(p => p.TypeId).SequenceEqual(new[] { "type:int32", "type:int32", entry.DelegateTypeId })
                || callable.Parameters.Any(p => p.RefKind != "none")
                || signature is null || signature.ReturnRefKind != "none"
                || signature.Parameters.Any(p => p.RefKind is not ("none" or "ref" or "out"))) return false;
            if (document.SchemaVersion < 30)
            {
                if (entry.EventSymbolId is not null || entry.OwnerTypeId is not null) return false;
                continue;
            }
            if ((entry.EventSymbolId is null) != (entry.OwnerTypeId is null)) return false;
            if (entry.EventSymbolId is null) continue;
            if (!languageSymbols.Add(entry.EventSymbolId)) return false;
            SemanticSymbol? eventSymbol = document.Symbols.SingleOrDefault(symbol => symbol.Id == entry.EventSymbolId);
            SemanticSymbol? ownerSymbol = document.Symbols.SingleOrDefault(symbol => symbol.Id == "symbol:" + entry.OwnerTypeId);
            if (eventSymbol is null || eventSymbol.Kind != "event" || eventSymbol.IsStatic
                || eventSymbol.Accessibility != "public" || !eventSymbol.IsExecutableReferenceSource
                || eventSymbol.TypeId != entry.DelegateTypeId
                || eventSymbol.ContainingSymbolId != ownerSymbol?.Id
                || ownerSymbol?.TypeId != entry.OwnerTypeId) return false;
        }
        Dictionary<string, SemanticEventSubscription> languageEvents = document.EventSubscriptions
            .Where(entry => entry.EventSymbolId is not null)
            .ToDictionary(entry => entry.EventSymbolId!, StringComparer.Ordinal);
        IEnumerable<SemanticOperation> roots = document.Methods.Select(method => method.Root)
            .Concat(document.ControlFlowGraphs.SelectMany(graph => graph.Blocks)
                .SelectMany(block => block.Operations.Concat(block.BranchValue is null
                    ? Array.Empty<SemanticOperation>() : new[] { block.BranchValue })))
            .Concat(document.AsyncMethods.SelectMany(method => method.Segments)
                .SelectMany(segment => segment.Statements.Select(statement => statement.Operation)
                    .Concat(segment.Transfer?.Condition is { } condition ? new[] { condition } : Array.Empty<SemanticOperation>())
                    .Concat(segment.AwaitSite?.Arguments ?? Array.Empty<SemanticOperation>())
                    .Concat(segment.AwaitSite?.CancellationToken is { } cancellation
                        ? new[] { cancellation } : Array.Empty<SemanticOperation>())));
        return roots.SelectMany(EnumerateOperations).All(operation =>
        {
            if (operation.Kind != "event_assignment") return true;
            if (document.SchemaVersion < 30 || operation.OperatorKind is not ("add" or "remove")
                || operation.SymbolId is null || !languageEvents.TryGetValue(operation.SymbolId, out var entry)
                || operation.Children.Count != 2) return false;
            SemanticOperation eventReference = operation.Children[0];
            SemanticOperation handler = operation.Children[1];
            return eventReference.Kind == "event_reference"
                && eventReference.SymbolId == entry.EventSymbolId
                && eventReference.TypeId == entry.DelegateTypeId
                && eventReference.Children.Count == 1
                && handler.TypeId == entry.DelegateTypeId;
        });
    }

    private static IEnumerable<SemanticOperation> EnumerateOperations(SemanticOperation operation)
    {
        yield return operation;
        foreach (SemanticOperation child in operation.Children)
        {
            foreach (SemanticOperation nested in EnumerateOperations(child)) yield return nested;
        }
    }
}
