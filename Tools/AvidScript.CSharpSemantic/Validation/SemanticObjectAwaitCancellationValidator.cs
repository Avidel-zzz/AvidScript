using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;

namespace AvidScript.CSharpSemantic;

// Checks producer/payload identity independently of projection. Scope dispatch,
// task ownership and state-frame liveness remain owned by their validators.
public static class SemanticObjectAwaitCancellationValidator
{
    public static bool IsValid(SemanticDocument? document)
    {
        if (document?.AsyncMethods is null || document.Types is null || document.Symbols is null
            || document.AsyncMethods.Any(method => method?.Segments is null
                || method.Segments.Any(segment => segment is null))) return false;
        if (!SemanticObjectAwaitCancellation.IsVersion(document))
            return document.SchemaVersion != SemanticObjectAwaitCancellation.SchemaVersion
                && document.SemanticVersion != SemanticObjectAwaitCancellation.SemanticVersion
                && document.AsyncMethods.All(method => method.Segments.All(segment =>
                    segment.AwaitSite?.ProducerKind != SemanticObjectAwaitCancellation.ProducerKind
                    || segment.Transfer?.CancellationTarget is null));
        if (!SemanticComposableCapabilityValidator.IsValid(document)
            || !SemanticObjectAwaitCancellation.Has(document)) return false;
        HashSet<int> callbackIds = new();
        int objectSites = 0;
        foreach (SemanticAsyncMethod method in document.AsyncMethods)
        {
            foreach (SemanticAsyncSegment segment in method.Segments)
            {
                if (segment.AwaitSite is not { } site) continue;
                if (site.CallbackId < SemanticContinuationCallback.CompilerCallbackIdStart
                    || !callbackIds.Add(site.CallbackId)) return false;
                if (!SemanticObjectAwaitCancellation.IsStatusAwareSite(document, method, site)) continue;
                if (++objectSites > SemanticObjectAwaitCancellation.MaximumAwaitSites
                    || method.Lowering != SemanticAsyncMethod.ContinuationCfgLowering
                    || segment.Transfer is not { Kind: SemanticAsyncMethod.AwaitTransferKind,
                        PrimaryTarget: >= 0, SecondaryTarget: -1, ExceptionTypeId: null }
                    || site.PayloadKind != SemanticContinuationCallback.ObjectPayloadKind
                    || site.BindingOrdinal != -1 || site.PayloadDescriptorTypeId is not null
                    || site.PayloadValueTypeId is not null || site.TaskCallableId is not null
                    || site.TaskLocalSymbolId is not null || site.ResultStorageKind is not null
                    || site.MemberAssignment is not null
                    || site.Arguments is not { Count: 1 }
                    || site.Arguments[0] is not { IsSupported: true, TypeId: "type:string",
                        Constant: { Kind: "string", Value: { } path } }
                    || !ValidAssetPath(path)
                    || (site.ResultSymbolId is null) != (site.ResultTypeId is null)) return false;
                if (site.ResultSymbolId is { } result
                    && (document.Types.SingleOrDefault(type => type.Id == site.ResultTypeId) is not {
                            CanonicalName: SemanticObjectAwaitCancellation.LoadedObjectTypeName,
                            Kind: "struct", IsValueType: true, IsNullable: false }
                        || !document.Symbols.Any(symbol => symbol.Id == result && symbol.Kind == "local"
                            && symbol.TypeId == site.ResultTypeId
                            && symbol.ContainingSymbolId == method.MethodSymbolId))) return false;
            }
        }
        return objectSites > 0;
    }

    private static bool ValidAssetPath(string path)
    {
        if (path.Length < 4 || path[0] != '/' || path[1] == '/' || path.IndexOf('\\') >= 0
            || path.Any(character => char.IsControl(character) || char.IsWhiteSpace(character))
            || path.Contains("//", StringComparison.Ordinal) || Encoding.UTF8.GetByteCount(path) > 1024) return false;
        int slash = path.LastIndexOf('/');
        int separator = path.IndexOf('.', slash + 1);
        return slash > 0 && separator > slash + 1 && separator < path.Length - 1
            && path.IndexOf('.', separator + 1) < 0;
    }
}
