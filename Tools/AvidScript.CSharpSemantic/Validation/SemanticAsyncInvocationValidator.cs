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
            || document.Types is null || document.TypeShapes is null
            || document.Callables.Any(callable => callable is null || string.IsNullOrWhiteSpace(callable.MethodSymbolId))
            || document.Callables.Select(callable => callable.MethodSymbolId).Distinct(StringComparer.Ordinal).Count() != document.Callables.Count)
            return false;
        if (document.SchemaVersion == SemanticContract.TaskResultSchemaVersion
            && document.SemanticVersion == SemanticContract.TaskResultSemanticVersion
            && !document.AsyncMethods.Any(method => method?.TaskResultTypeId is not null
                || method?.Segments?.Any(segment => segment?.AwaitSite?.TaskCallableId is not null) == true))
            return false;
        var callables = document.Callables.ToDictionary(callable => callable.MethodSymbolId, StringComparer.Ordinal);
        foreach (SemanticAsyncMethod method in document.AsyncMethods)
        {
            if (method is null || string.IsNullOrWhiteSpace(method.MethodSymbolId) || method.InvocationInputs is null
                || !callables.TryGetValue(method.MethodSymbolId, out SemanticCallable? callable)
                || callable.Parameters is null || callable.Parameters.Any(parameter => parameter is null
                    || string.IsNullOrWhiteSpace(parameter.SymbolId) || string.IsNullOrWhiteSpace(parameter.TypeId))
                || !callable.HasBody || callable.IsConstructor || callable.Import is not null
                || callable.Export?.Name != method.ExportName
                || (method.TaskResultTypeId is null
                    ? callable.ReturnTypeId != "type:void"
                    : !IsSupportedTaskResult(document, callable.ReturnTypeId, method.TaskResultTypeId)
                        || method.ExportName is not null))
                return false;
            if (method.TaskResultTypeId is not null
                && (document.SchemaVersion != SemanticContract.TaskResultSchemaVersion
                    || document.SemanticVersion != SemanticContract.TaskResultSemanticVersion
                    || method.Lowering != SemanticAsyncMethod.ContinuationCfgLowering)) return false;
            if (document.SchemaVersion < 28)
            {
                if (method.InvocationInputs.Count != 0 || method.ExportName is null
                    || !callable.IsStatic || callable.Parameters.Count != 0) return false;
                continue;
            }
            if (!(document.SchemaVersion == 28 && document.SemanticVersion == "1.32")
                && !(document.SchemaVersion == 29 && document.SemanticVersion == "1.33")
                && !SemanticContract.IsCurrentOrPrevious(document.SchemaVersion, document.SemanticVersion)) return false;
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
            if (method.TaskResultTypeId is not null
                && method.Segments.Any(segment => segment.Transfer is
                    { Kind: SemanticAsyncMethod.ReturnTransferKind, Condition: { } value }
                    && value.TypeId != method.TaskResultTypeId)) return false;
            foreach (SemanticAsyncAwaitSite site in method.Segments.Where(segment => segment.AwaitSite is not null).Select(segment => segment.AwaitSite!))
            {
                if (expected.Length != 0 && (site.StateFrame?.Slots is null
                    || expected.Any(slot => site.StateFrame.Slots.Count(candidate => candidate == slot) != 1))) return false;
                if (site.ProducerKind == "task_call")
                {
                    if (document.SchemaVersion != SemanticContract.TaskResultSchemaVersion
                        || document.SemanticVersion != SemanticContract.TaskResultSemanticVersion
                        || site.TaskCallableId is null
                        || !callables.TryGetValue(site.TaskCallableId, out SemanticCallable? target)
                        || !target.IsStatic || target.Parameters.Count != 0
                        || !IsSupportedTaskResult(document, target.ReturnTypeId, site.ResultTypeId ?? "")
                        || !document.AsyncMethods.Any(producer => producer.MethodSymbolId == site.TaskCallableId
                            && producer.TaskResultTypeId == site.ResultTypeId)
                        || site.PayloadKind != "task_result"
                        || site.PayloadValueTypeId != site.ResultTypeId
                        || site.ResultSymbolId is { } resultSymbolId
                            && !document.Symbols.Any(symbol => symbol?.Id == resultSymbolId
                                && symbol.Kind == "local" && symbol.TypeId == site.ResultTypeId)
                        || site.Arguments is null || site.Arguments.Count != 0
                        || site.CancellationToken is not null
                        || site.BindingOrdinal != -1
                        || site.PayloadDescriptorTypeId is not null) return false;
                }
                else if (site.TaskCallableId is not null) return false;
            }
        }
        return true;
    }

    private static bool IsSupportedTaskResult(
        SemanticDocument document, string returnTypeId, string resultTypeId)
    {
        return resultTypeId == "type:int32"
            && document.Types.Any(type => type?.Id == returnTypeId
                && type.CanonicalName == "global::System.Threading.Tasks.Task<int>")
            && document.TypeShapes.Any(shape => shape?.TypeId == returnTypeId
                && shape.GenericArgumentTypeIds is { Count: 1 }
                && shape.GenericArgumentTypeIds[0] == resultTypeId);
    }

    public static SemanticAsyncStateSlot[] MergeStateSlots(IReadOnlyList<SemanticAsyncStateSlot> locals,
        IReadOnlyList<SemanticAsyncStateSlot> inputs) => locals.Concat(inputs)
        .OrderBy(slot => slot.SymbolId, StringComparer.Ordinal).ToArray();
}
