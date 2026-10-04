using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.CompilerServices;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

// Only source validation can create this context. Serialized metadata cannot
// authorize lowering or publication of a private composition intermediate.
internal sealed class CSharpObjectAwaitExecutionContext
{
    private static readonly ConditionalWeakTable<SemanticDocument, CSharpObjectAwaitExecutionContext> Contexts = new();
    private readonly SemanticCapabilityManifest sourceManifest;
    private readonly int[] callbacks;

    private CSharpObjectAwaitExecutionContext(SemanticDocument source)
    {
        sourceManifest = source.CapabilityManifest!;
        callbacks = source.AsyncMethods.SelectMany(method => method.Segments
            .Where(segment => segment.AwaitSite is { } site
                && SemanticObjectAwaitCancellation.IsStatusAwareSite(source, method, site))
            .Select(segment => segment.AwaitSite!.CallbackId)).Order().ToArray();
    }

    internal static bool TryCreate(SemanticDocument source, out SemanticDocument? execution,
        out CSharpObjectAwaitExecutionContext? context)
    {
        execution = null; context = null;
        if (!SemanticObjectAwaitCancellation.Has(source)
            || !SemanticObjectAwaitCancellationValidator.IsValid(source)
            || !SemanticAsyncInvocationValidator.IsValid(source)
            || !SemanticAsyncScopeValidator.IsValid(source)
            || !SemanticExceptionFlowContractValidator.IsValid(source)
            || source.StaticInitialization is not null && !SemanticStaticInitializationValidator.IsValid(source)) return false;
        execution = source with { };
        context = new(source);
        context.Attach(execution);
        return true;
    }

    internal void Attach(SemanticDocument document)
    {
        if (!Contexts.TryGetValue(document, out _)) Contexts.Add(document, this);
    }

    internal static CSharpObjectAwaitExecutionContext? Find(SemanticDocument document) =>
        Contexts.TryGetValue(document, out var context) ? context : null;

    internal GuestModule Wrap(GuestModule input)
    {
        var capabilities = new List<GuestCapability> { new(GuestObjectAwaitCancellation.CapabilityId, 1) };
        if (input.StaticStorage is not null) capabilities.Add(new(GuestComposableCapabilities.StaticStorage, 1));
        if (input.DirectAwaitReadiness is not null) capabilities.Add(new(GuestComposableCapabilities.AwaitReadiness, 1));
        if (input.CancellationIdentity is not null) capabilities.Add(new(GuestComposableCapabilities.CancellationIdentity, 1));
        if (input.ExceptionValues is not null) capabilities.Add(new(GuestComposableCapabilities.ExceptionValues, 1));
        if (input.CancellationTokens is not null) capabilities.Add(new(GuestComposableCapabilities.CancellationTokenValue, 1));
        if (input.AsyncVoidErrorOwners is not null) capabilities.Add(new(GuestAsyncVoidErrorOwners.CapabilityId, 1));
        return input with {
            SchemaVersion = GuestObjectAwaitCancellation.SchemaVersion, IrVersion = GuestObjectAwaitCancellation.IrVersion,
            Provenance = input.Provenance with { SemanticSchemaVersion = GuestObjectAwaitCancellation.SemanticSchemaVersion,
                SemanticVersion = GuestObjectAwaitCancellation.SemanticVersion },
            CapabilityManifest = GuestCapabilityManifest.Create(GuestObjectAwaitCancellation.ExecutionBaseSchemaVersion,
                GuestObjectAwaitCancellation.ExecutionBaseIrVersion, capabilities),
            ObjectAwaitCancellation = new(sourceManifest.BaseSchemaVersion, sourceManifest.BaseSemanticVersion, callbacks),
            StaticAsyncValueComposition = null, AsyncVoidComposition = null,
            DirectAwaitReadiness = input.DirectAwaitReadiness is { } ready ? ready with {
                BaseSchemaVersion = GuestObjectAwaitCancellation.ExecutionBaseSchemaVersion,
                BaseIrVersion = GuestObjectAwaitCancellation.ExecutionBaseIrVersion } : null,
            CancellationIdentity = input.CancellationIdentity is { } identity ? identity with {
                BaseSchemaVersion = GuestObjectAwaitCancellation.ExecutionBaseSchemaVersion,
                BaseIrVersion = GuestObjectAwaitCancellation.ExecutionBaseIrVersion } : null,
        };
    }
}
