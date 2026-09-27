using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.CompilerServices;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

// Attached only to compiler-owned document copies. No caller can opt an
// arbitrary intermediate IR into publication by setting a serialized flag.
internal sealed class CSharpCancellationTokenExecutionContext
{
    private static readonly ConditionalWeakTable<SemanticDocument, CSharpCancellationTokenExecutionContext> Contexts = new();
    internal bool HasAsync { get; }
    internal bool NeedsErrors { get; }
    internal bool HasReader { get; }
    private CSharpCancellationTokenExecutionContext(SemanticDocument source)
    {
        HasAsync = source.AsyncMethods.Count != 0;
        HasReader = source.Methods.SelectMany(method => Operations(method.Root))
            .Any(operation => operation.Kind == SemanticCancellationTokens.Read);
        NeedsErrors = HasAsync || HasReader || source.ExceptionFlows is { Count: > 0 };
    }

    internal static bool TryCreate(SemanticDocument source, out SemanticDocument? execution,
        out CSharpCancellationTokenExecutionContext? context)
    {
        execution = null;
        context = null;
        if (source.Source is null || source.Diagnostics is null || source.Diagnostics.Any(item => item is null)
            || source.Callables is null || source.Callables.Any(item => item is null)
            || source.Symbols is null || source.Symbols.Any(item => item is null)
            || source.ClassTypes is null || source.ClassTypes.Any(item => item is null)
            || source.TypeShapes is null || source.TypeShapes.Any(item => item is null)
            || !SemanticContract.HasCancellationTokens(source) || !SemanticCancellationTokenValidator.IsValid(source)
            || !SemanticExceptionFlowContractValidator.IsValid(source) || !SemanticAsyncScopeValidator.IsValid(source)) return false;
        var guards = CSharpAsyncMemberAssignmentLowerer.GuardSites(source);
        if (guards.Select(guard => guard.Id).Distinct(StringComparer.Ordinal).Count() != guards.Count) return false;
        execution = source with { };
        context = new(source);
        context.Attach(execution);
        return true;
    }

    internal void Attach(SemanticDocument document)
    {
        if (!Contexts.TryGetValue(document, out _)) Contexts.Add(document, this);
    }
    internal static CSharpCancellationTokenExecutionContext? Find(SemanticDocument document) =>
        Contexts.TryGetValue(document, out var context) ? context : null;

    internal GuestModule Wrap(GuestModule input) => input with {
        SchemaVersion = GuestCancellationTokens.SchemaVersion, IrVersion = GuestCancellationTokens.IrVersion,
        Provenance = input.Provenance with { SemanticSchemaVersion = GuestCancellationTokens.SemanticSchemaVersion,
            SemanticVersion = GuestCancellationTokens.SemanticVersion },
        CancellationTokens = HasAsync ? new(29, "1.28") : NeedsErrors ? new(17, "1.16") : new(14, "1.13"),
    };

    internal static IEnumerable<SemanticOperation> Operations(SemanticOperation root)
    {
        var pending = new Stack<SemanticOperation>();
        var seen = new HashSet<SemanticOperation>(ReferenceEqualityComparer.Instance);
        pending.Push(root);
        while (pending.TryPop(out var operation)) {
            if (!seen.Add(operation)) continue;
            yield return operation;
            foreach (var child in operation.Children) pending.Push(child);
        }
    }
}
