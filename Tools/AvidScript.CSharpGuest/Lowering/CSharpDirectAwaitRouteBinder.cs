using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal static class CSharpDirectAwaitRouteBinder
{
    internal static bool TryBind(GuestModule module, out GuestModule bound, out string? error)
    {
        bound = module;
        error = null;
        if (module.DirectAwaitRoutes is not { Count: > 0 } routes) return true;
        List<GuestDirectAwaitRoute> rewritten = new();
        foreach (var route in routes)
        {
            // Conditions and outcome guards split the source segment before its
            // scheduling call. Bind the published route to the actual instruction
            // block, including every resume entry that can reach this await.
            var owners = module.Functions.Where(function => function.Blocks.Any(block => block.Id == route.AwaitBlockId)).ToArray();
            HashSet<string> scheduleBlocks = new(StringComparer.Ordinal);
            foreach (var function in owners)
            {
                var matches = function.Blocks.Where(block => block.Id == route.AwaitBlockId
                        || block.Id.StartsWith(route.AwaitBlockId + ":", StringComparison.Ordinal))
                    .SelectMany(block => block.Instructions.Where(instruction => instruction.Op == "call"
                            && instruction.TargetId == route.ScheduleImportId)
                        .Select(instruction => (Block: block, Call: instruction))).ToArray();
                if (matches.Length != 1 || matches[0].Call.OperandIds.Count != 2
                    || function.Blocks.SelectMany(block => block.Instructions).Count(instruction =>
                        instruction.ResultId == matches[0].Call.OperandIds[1]
                        && instruction.Op == "constant" && instruction.Constant is { Kind: "int32" } literal
                        && literal.Value == route.CallbackId.ToString(System.Globalization.CultureInfo.InvariantCulture)) != 1)
                {
                    error = $"Direct await {route.CallbackId} needs one scheduling call with its exact callback identity in every entry.";
                    return false;
                }
                scheduleBlocks.Add(matches[0].Block.Id);
            }
            if (owners.Length == 0 || scheduleBlocks.Count != 1)
            {
                error = $"Direct await {route.CallbackId} has missing or inconsistent scheduling blocks across entries.";
                return false;
            }
            rewritten.Add(route with { AwaitBlockId = scheduleBlocks.Single() });
        }
        bound = module with { DirectAwaitRoutes = rewritten };
        return true;
    }
}
