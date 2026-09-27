using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

// Explicit transport upgrade; it does not enable new C# syntax or change the
// default lowerer. Both the input and the resulting envelope must validate.
public static class CSharpCancellationIdentityCompiler
{
    public static bool TryUpgrade(GuestModule input, out GuestModule? module, out string? error)
    {
        module = null;
        error = null;
        var validation = GuestModuleValidator.Validate(input);
        if (!validation.Succeeded)
        {
            error = "Invalid cancellation input: " + string.Join(" | ", validation.Diagnostics.Select(d => d.Code + ": " + d.Message));
            return false;
        }
        if (!TryUpgradeCore(input, out var candidate, out error)) return false;
        validation = GuestModuleValidator.Validate(candidate!);
        if (!validation.Succeeded)
        {
            error = "Invalid cancellation identity output: " + string.Join(" | ", validation.Diagnostics.Select(d => d.Code + ": " + d.Message));
            return false;
        }
        module = candidate;
        return true;
    }

    // Only compiler-owned source composition can pass through a module whose
    // token layout belongs to the final IR34 contract. Publication still runs
    // all independent validators after the identity/catch plans are complete.
    internal static bool TryUpgradeForTokens(CSharpCancellationTokenExecutionContext context, GuestModule input,
        out GuestModule? module, out string? error)
    {
        module = null;
        error = null;
        if (!context.HasAsync) { error = "Token identity composition requires an async execution context."; return false; }
        return TryUpgradeCore(input, out module, out error);
    }

    private static bool TryUpgradeCore(GuestModule input, out GuestModule? module, out string? error)
    {
        module = null;
        error = null;
        if (GuestTaskCancellationIdentity.IsVersion(input))
        {
            module = input;
            return true;
        }
        var profile = GuestDirectAwaitReadiness.BaseProfile(input);
        if (input.Language != "csharp" || !GuestTaskCancellationIdentity.IsBase(profile.SchemaVersion, profile.IrVersion)
            || input.LanguageErrorCatalog is not { Types.Count: > 0, Sources.Count: > 0 }
            || input.Imports.Count(import => import.Id == GuestTaskCancellationErrorValidator.ImportId) != 1)
        {
            error = "Cancellation identity needs a C# cancellation execution profile with a complete language error catalog.";
            return false;
        }
        var readyProofs = new Dictionary<(string Function, string Block), string>();
        foreach (var guard in input.DirectAwaitReadiness?.Guards ?? Array.Empty<GuestDirectAwaitReadinessGuard>())
        {
            var query = input.Functions.Single(function => function.Id == guard.FunctionId).Blocks
                .Single(block => block.Id == guard.CheckBlockId).Instructions.Single(instruction =>
                    instruction.Op == "call" && instruction.TargetId == GuestDirectAwaitReadiness.ImportId);
            readyProofs.Add((guard.FunctionId, guard.CancellationBlockId + ":task_created"), query.OperandIds[0]);
        }
        var pending = (input.DirectAwaitRoutes ?? Array.Empty<GuestDirectAwaitRoute>()).Select(route =>
            ("function:synthetic:async_resume:" + route.CallbackId,
                route.NormalTargetBlockId + ":entry:cancel_path:task_created")).ToHashSet();
        var functions = new List<GuestFunction>();
        foreach (var function in input.Functions)
        {
            var locals = function.Locals.ToList();
            var names = function.Parameters.Concat(function.Locals).Select(register => register.Id).ToHashSet(StringComparer.Ordinal);
            var blocks = new List<GuestBasicBlock>();
            int next = 0;
            foreach (var block in function.Blocks)
            {
                var instructions = new List<GuestInstruction>();
                foreach (var instruction in block.Instructions)
                {
                    if (instruction.Op != "call" || instruction.TargetId != GuestTaskCancellationErrorValidator.ImportId)
                    {
                        instructions.Add(instruction);
                        continue;
                    }
                    if (!readyProofs.TryGetValue((function.Id, block.Id), out string? proof))
                    {
                        if (!pending.Contains((function.Id, block.Id)) || instructions.Count < 2)
                        {
                            error = "Cancellation producer has no checked source route: " + function.Id + "/" + block.Id;
                            return false;
                        }
                        do { proof = "register:cancel_identity:" + next++; } while (!names.Add(proof));
                        locals.Add(new(proof, "type:int64"));
                        // Keep allocation, payload initialization and publication adjacent.
                        instructions.Insert(instructions.Count - 2,
                            new("constant", proof, Array.Empty<string>(), null, null, new("int64", "0")));
                    }
                    instructions.Add(instruction with
                    {
                        TargetId = GuestTaskCancellationIdentity.CancelImportId,
                        OperandIds = instruction.OperandIds.Append(proof).ToArray(),
                    });
                }
                blocks.Add(block with { Instructions = instructions });
            }
            functions.Add(function with { Locals = locals, Blocks = blocks });
        }
        var candidate = input with
        {
            SchemaVersion = GuestTaskCancellationIdentity.SchemaVersion,
            IrVersion = GuestTaskCancellationIdentity.IrVersion,
            CancellationIdentity = new(profile.SchemaVersion, profile.IrVersion),
            Functions = functions,
            Imports = input.Imports.Select(import => import.Id == GuestTaskCancellationErrorValidator.ImportId
                ? import with { Id = GuestTaskCancellationIdentity.CancelImportId,
                    Name = GuestTaskCancellationIdentity.CancelImportName,
                    ParameterTypeIds = import.ParameterTypeIds.Append("type:int64").ToArray() }
                : import).Append(new GuestImport(GuestTaskCancellationIdentity.ReadImportId, "avidscript",
                    GuestTaskCancellationIdentity.ReadImportName, new[] { "type:int64" }, "type:int64")).ToArray(),
        };
        module = candidate;
        return true;
    }
}
