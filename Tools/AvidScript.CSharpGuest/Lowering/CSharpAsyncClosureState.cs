using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;

namespace AvidScript.CSharpGuest;

// An execution layout derived after validating the source Semantic contract.
// Captured cells live in their environments, never in value snapshots that could
// overwrite shared writes when a continuation resumes.
internal static class CSharpAsyncClosureState
{
    public static IEnumerable<SemanticAsyncStateFrame> Frames(SemanticDocument document) => document.AsyncMethods
        .SelectMany(method => method.Segments.Where(segment => segment.AwaitSite is not null)
            .Select(segment => Frame(document, method, segment.AwaitSite!))).OfType<SemanticAsyncStateFrame>();

    public static SemanticAsyncStateFrame? Frame(SemanticDocument document, SemanticAsyncMethod method, SemanticAsyncAwaitSite site)
    {
        SemanticClosureEnvironment[] owned = document.ClosureEnvironments.Where(environment => environment.OwnerMethodSymbolId == method.MethodSymbolId).ToArray();
        if (owned.Length == 0 && method.TaskResultTypeId is null
            && site.ProducerKind != "task_call") return site.StateFrame;
        int segment = method.Segments.Single(segment => segment.AwaitSite?.CallbackId == site.CallbackId).Ordinal;
        // this is immutable and still needed by ordinary instance accesses and
        // the resume authority check, even when a closure also captures it.
        HashSet<string> captured = owned.SelectMany(environment => environment.Cells).Where(cell => cell.Kind != "receiver")
            .Select(cell => cell.SymbolId).ToHashSet(StringComparer.Ordinal);
        HashSet<string> active = method.LexicalScopes.Where(scope => scope.Segments.Contains(segment)).Select(scope => scope.Id).ToHashSet(StringComparer.Ordinal);
        SemanticAsyncStateSlot[] slots = (site.StateFrame?.Slots ?? Array.Empty<SemanticAsyncStateSlot>())
            .Where(slot => !captured.Contains(slot.SymbolId))
            .Concat(owned.Where(environment => active.Contains(environment.Id))
                .Select(environment => new SemanticAsyncStateSlot(environment.Id, CSharpClosureLayout.Reference(environment.Id))))
            .Concat(method.TaskResultTypeId is null ? Array.Empty<SemanticAsyncStateSlot>() : new[]
            {
                new SemanticAsyncStateSlot(CSharpTaskResultAbi.ProducerSlot(method), CSharpTaskResultAbi.TokenTypeId),
            })
            .Concat(site.ProducerKind != "task_call" ? Array.Empty<SemanticAsyncStateSlot>() : new[]
            {
                new SemanticAsyncStateSlot(CSharpTaskResultAbi.AwaitSlot(site), CSharpTaskResultAbi.TokenTypeId),
            })
            .OrderBy(slot => slot.SymbolId, StringComparer.Ordinal).ToArray();
        if (slots.Length == 0) return null;
        return new(site.StateFrame?.TypeId ?? "type:$async:closure_state:" + site.CallbackId, slots);
    }

    public static bool IsEnvironment(SemanticDocument document, SemanticAsyncStateSlot slot) =>
        document.ClosureEnvironments.Any(environment => environment.Id == slot.SymbolId && CSharpClosureLayout.Reference(environment.Id) == slot.TypeId);
}
