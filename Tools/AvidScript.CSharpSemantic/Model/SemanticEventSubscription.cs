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
    [property: JsonPropertyOrder(3), JsonRequired] string DelegateTypeId)
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
        }
        return true;
    }
}
