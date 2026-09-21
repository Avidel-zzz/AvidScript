using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.CSharpSemantic;

// The same reader contract protects downstream consumers and independently
// verifies that no caller stack reference becomes a persistent async input.
public static class SemanticAsyncInvocationValidator
{
    public static bool IsValid(SemanticDocument document)
    {
        if (document.AsyncMethods is null || document.Callables is null || document.Symbols is null
            || document.Types is null || document.Callables.Any(callable => callable is null || string.IsNullOrWhiteSpace(callable.MethodSymbolId))
            || document.Callables.Select(callable => callable.MethodSymbolId).Distinct(StringComparer.Ordinal).Count() != document.Callables.Count)
            return false;
        var callables = document.Callables.ToDictionary(callable => callable.MethodSymbolId, StringComparer.Ordinal);
        foreach (SemanticAsyncMethod method in document.AsyncMethods)
        {
            if (method is null || string.IsNullOrWhiteSpace(method.MethodSymbolId) || method.InvocationInputs is null
                || !callables.TryGetValue(method.MethodSymbolId, out SemanticCallable? callable)
                || callable.Parameters is null || callable.Parameters.Any(parameter => parameter is null
                    || string.IsNullOrWhiteSpace(parameter.SymbolId) || string.IsNullOrWhiteSpace(parameter.TypeId))
                || !callable.HasBody || callable.IsConstructor || callable.Import is not null
                || callable.ReturnTypeId != "type:void" || callable.Export?.Name != method.ExportName)
                return false;
            if (document.SchemaVersion < 28)
            {
                if (method.InvocationInputs.Count != 0 || method.ExportName is null
                    || !callable.IsStatic || callable.Parameters.Count != 0) return false;
                continue;
            }
            if (document.SchemaVersion != SemanticContract.CurrentSchemaVersion
                || document.SemanticVersion != SemanticContract.CurrentSemanticVersion) return false;
            if (method.ExportName is not null)
            {
                if (string.IsNullOrWhiteSpace(method.ExportName) || !callable.IsStatic
                    || callable.Parameters.Count != 0 || method.InvocationInputs.Count != 0) return false;
                continue;
            }
            if (method.Lowering != SemanticAsyncMethod.ContinuationCfgLowering
                || callable.Parameters.Any(parameter => parameter.RefKind != "none")
                || method.InvocationInputs.Count > 64
                || !document.Types.Any(type => type?.Id == callable.ContainingTypeId && type.Kind == "class")) return false;
            var expected = callable.Parameters.Select(parameter => new SemanticAsyncStateSlot(parameter.SymbolId, parameter.TypeId))
                .Concat(callable.IsStatic ? Array.Empty<SemanticAsyncStateSlot>() : new[] {
                    new SemanticAsyncStateSlot(SemanticAsyncMethod.ReceiverSymbol(callable.MethodSymbolId), callable.ContainingTypeId) })
                .OrderBy(slot => slot.SymbolId, StringComparer.Ordinal).ToArray();
            if (!method.InvocationInputs.SequenceEqual(expected)
                || expected.Select(slot => slot.SymbolId).Distinct(StringComparer.Ordinal).Count() != expected.Length
                || expected.Any(slot => !document.Types.Any(type => type?.Id == slot.TypeId))
                || callable.Parameters.Any(parameter => !document.Symbols.Any(symbol => symbol?.Id == parameter.SymbolId
                    && symbol.Kind == "parameter" && symbol.TypeId == parameter.TypeId && symbol.ContainingSymbolId == callable.MethodSymbolId))) return false;
            if (method.Segments is null || method.Segments.Any(segment => segment is null)) return false;
            foreach (SemanticAsyncAwaitSite site in method.Segments.Where(segment => segment.AwaitSite is not null).Select(segment => segment.AwaitSite!))
                if (expected.Length != 0 && (site.StateFrame?.Slots is null
                    || expected.Any(slot => site.StateFrame.Slots.Count(candidate => candidate == slot) != 1))) return false;
        }
        return true;
    }

    public static SemanticAsyncStateSlot[] MergeStateSlots(IReadOnlyList<SemanticAsyncStateSlot> locals,
        IReadOnlyList<SemanticAsyncStateSlot> inputs) => locals.Concat(inputs)
        .OrderBy(slot => slot.SymbolId, StringComparer.Ordinal).ToArray();
}
