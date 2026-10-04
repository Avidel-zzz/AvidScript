using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

// Generated Host routers have no C# body or source span. Each affected callback
// crosses the same checked error boundary as a direct UE export; callback
// payloads and their existing public ABI remain unchanged.
internal static class CSharpGeneratedLanguageErrorAdapter
{
    private static readonly IReadOnlyDictionary<string, string> HostEntries = new Dictionary<string, string>(StringComparer.Ordinal)
    {
        [CSharpGuestIds.GameplayEventFunctionId] = CSharpGuestIds.GameplayEventExportName,
        [CSharpGuestIds.ContinuationFunctionId] = CSharpGuestIds.ContinuationExportName,
        [CSharpGuestIds.ContinuationV2FunctionId] = CSharpGuestIds.ContinuationV2ExportName,
        [CSharpGuestIds.UeBeginPlayCompatibilityFunctionId] = CSharpGuestIds.UeBeginPlayCompatibilityExportName,
        [CSharpGuestIds.UeTickCompatibilityFunctionId] = CSharpGuestIds.UeTickCompatibilityExportName,
        [CSharpGuestIds.UeEndPlayCompatibilityFunctionId] = CSharpGuestIds.UeEndPlayCompatibilityExportName,
    };

    internal static bool RequiresAdapter(GuestFunction function, IReadOnlySet<string> affected) =>
        HostEntries.ContainsKey(function.Id) && function.Blocks.SelectMany(block => block.Instructions)
            .Any(instruction => instruction.Op == "call" && instruction.TargetId is { } target && affected.Contains(target));

    internal static bool TryRestore(GuestModule module, GuestFunction router,
        IReadOnlyList<GuestExport> routerExports,
        IReadOnlyDictionary<string, GuestFunction> originalFunctions,
        IReadOnlySet<string> affected,
        out GuestModule? restored, out string? error)
    {
        restored = null;
        error = null;
        if (!HostEntries.TryGetValue(router.Id, out string? exportName) || router.ReturnTypeId != "type:void"
            || routerExports.Count != 1 || routerExports[0].FunctionId != router.Id
            || routerExports[0].Name != exportName
            || module.Functions.Any(function => function.Id == router.Id)
            || module.Exports.Any(export => export.Name == routerExports[0].Name))
        { error = "Generated outcomes require a unique compiler-owned Host router."; return false; }
        string[] targets = router.Blocks.SelectMany(block => block.Instructions)
            .Where(instruction => instruction.Op == "call" && instruction.TargetId is { } target && affected.Contains(target))
            .Select(instruction => instruction.TargetId!).Distinct(StringComparer.Ordinal)
            .OrderBy(id => id, StringComparer.Ordinal).ToArray();
        if (targets.Length == 0 || targets.Any(id => !originalFunctions.TryGetValue(id, out var original)
                || original.ReturnTypeId != "type:void"))
        { error = "Generated outcome callbacks require supported source-backed void methods."; return false; }
        // These specs name private adapters. They are never added to module.Exports.
        var boundaries = targets.Select(id => new GuestExport("generated:" + router.Id + ":" + id, id)).ToArray();
        if (!CSharpLanguageErrorEntryAdapter.TryAdd(module, boundaries, originalFunctions,
                out var adapted, out error, publishExports: false) || adapted is null) return false;
        var adapterIds = boundaries.ToDictionary(boundary => boundary.FunctionId,
            boundary => "function:language_error_entry:" + boundary.Name, StringComparer.Ordinal);
        GuestFunction routed = router with
        {
            Blocks = router.Blocks.Select(block => block with
            {
                Instructions = block.Instructions.Select(instruction => instruction.Op == "call"
                    && instruction.TargetId is { } target && adapterIds.TryGetValue(target, out string? adapter)
                        ? instruction with { TargetId = adapter } : instruction).ToArray(),
            }).ToArray(),
        };
        restored = adapted with
        {
            Functions = adapted.Functions.Append(routed).ToArray(),
            Exports = adapted.Exports.Concat(routerExports).OrderBy(export => export.Name, StringComparer.Ordinal).ToArray(),
        };
        return true;
    }
}
