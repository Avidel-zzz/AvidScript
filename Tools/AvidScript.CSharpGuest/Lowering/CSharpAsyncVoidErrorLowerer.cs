using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal static class CSharpAsyncVoidErrorLowerer
{
    internal static bool EmitPropagation(CSharpFunctionLoweringContext context, SemanticAsyncMethod method,
        int ordinal, string blockId, List<GuestInstruction> instructions, List<GuestBasicBlock> blocks)
    {
        if (method.VoidErrorOwner is null || method.TaskResultTypeId is not null
            || CSharpAsyncSynchronousExecutionContext.Find(context.Document) is not { HasVoidErrorOwners: true }) return false;
        var owner = CSharpAsyncExceptionLowerer.LoadOwner(context, method, ordinal, instructions);
        var payload = owner is null ? null : CSharpTaskResultAbi.ReadLanguageError(context, owner, ordinal, instructions);
        var reported = context.CreateTemporary("type:int32", ordinal);
        if (payload is null || reported is null) return false;
        instructions.Add(new("call", reported.Id,
            new[] { payload.TypeToken.Id, payload.SourceToken.Id, payload.Root.Id },
            CSharpTaskResultAbi.LanguageErrorReportImportId, null, null));
        string accepted = blockId + ":reported";
        string rejected = blockId + ":report_rejected";
        blocks.Add(new(blockId, instructions, new("branch_if", reported.Id, accepted, rejected, null)));
        blocks.Add(new(rejected, Array.Empty<GuestInstruction>(), new("trap", null, null, null, null)));
        instructions = new();
        if (!CSharpAsyncExceptionLowerer.ReleaseIfHeld(context, method, ordinal, blocks, ref accepted, ref instructions)
            || !CSharpTaskResultAbi.ReleaseTaskLocal(context, method, ordinal, instructions)) return false;
        blocks.Add(new(accepted, instructions, new("return", null, null, null, null)));
        return true;
    }

    internal static GuestModule Wrap(SemanticDocument source, GuestModule module)
    {
        var owners = source.AsyncMethods.Where(method => method.VoidErrorOwner is not null).Select(method =>
        {
            string exit = CSharpGuestIds.AsyncSegmentBlock(method.MethodSymbolId,
                method.VoidErrorOwner!.UnhandledExitSegmentOrdinal);
            string report = exit + ":propagate_exception";
            return new GuestAsyncVoidErrorOwner(CSharpGuestIds.Function(method.MethodSymbolId),
                CSharpGuestIds.Local(CSharpTaskResultAbi.ExceptionSourceSlot(method)),
                CSharpGuestIds.Local(CSharpTaskResultAbi.ExceptionTypeSlot(method)), exit,
                module.Functions.Where(function => function.Blocks.Any(block => block.Id == report))
                    .OrderBy(function => function.Id, StringComparer.Ordinal)
                    .Select(function => new GuestAsyncVoidErrorReport(function.Id, report)).ToArray());
        }).OrderBy(owner => owner.MethodFunctionId, StringComparer.Ordinal).ToArray();
        var capabilities = new List<GuestCapability> { new(GuestAsyncVoidErrorOwners.CapabilityId, 1) };
        if (module.StaticStorage is not null) capabilities.Add(new(GuestComposableCapabilities.StaticStorage, 1));
        if (module.DirectAwaitReadiness is not null) capabilities.Add(new(GuestComposableCapabilities.AwaitReadiness, 1));
        if (module.CancellationIdentity is not null) capabilities.Add(new(GuestComposableCapabilities.CancellationIdentity, 1));
        if (module.ExceptionValues is not null) capabilities.Add(new(GuestComposableCapabilities.ExceptionValues, 1));
        if (module.CancellationTokens is not null) capabilities.Add(new(GuestComposableCapabilities.CancellationTokenValue, 1));
        return module with {
            SchemaVersion = GuestAsyncVoidErrorOwners.SchemaVersion,
            IrVersion = GuestAsyncVoidErrorOwners.IrVersion,
            AsyncVoidErrorOwners = new(owners),
            CapabilityManifest = GuestCapabilityManifest.Create(29, "1.28", capabilities),
            Provenance = module.Provenance with {
                SemanticSchemaVersion = source.SchemaVersion, SemanticVersion = source.SemanticVersion },
        };
    }
}
