using System;
using System.Linq;

namespace AvidScript.GuestIr;

public static class GuestTaskLocalLifetimeValidator
{
    public const int SchemaVersion = 25;
    public const string IrVersion = "1.24";
    public const int SemanticSchemaVersion = 45;
    public const string SemanticVersion = "1.54";
    public const string RetainFunctionId = "function:$async:task_owner:retain";
    public const string ReleaseFunctionId = "function:$async:task_owner:release";
    public const string TransferFunctionId = "function:$async:task_owner:transfer";
    private const string DiagnosticCode = "ASIR1033";

    public static bool IsVersion(GuestModule module) => module.SchemaVersion == SchemaVersion && module.IrVersion == IrVersion;
    internal static bool HasErrors(GuestModule module) => IsVersion(module)
        && module.TaskLocalLifetimes?.ExceptionModel is "fault" or "exception" or "cleanup" or "cancellation";
    internal static bool HasExceptionFlow(GuestModule module) => IsVersion(module)
        && module.TaskLocalLifetimes?.ExceptionModel is "exception" or "cleanup" or "cleanup_only" or "cancellation";
    internal static bool HasDirectCleanup(GuestModule module) => IsVersion(module)
        && module.TaskLocalLifetimes?.ExceptionModel is "cleanup" or "cleanup_only" or "cancellation";
    internal static bool HasCancellation(GuestModule module) => IsVersion(module)
        && module.TaskLocalLifetimes?.ExceptionModel == "cancellation";

    // Route validators still prove the original exception protocol. They may
    // cross exactly one independently checked, release-only scope-exit block.
    internal static GuestFunction ResolveScopeExitRoutes(GuestModule module, GuestFunction function)
    {
        if (!IsVersion(module) || module.TaskLocalLifetimes?.Functions is not { } entries) return function;
        var owners = entries.Where(entry => entry?.FunctionId == function.Id).ToArray();
        if (owners.Length != 1 || owners[0].Releases is null) return function;
        string? Resolve(string? target)
        {
            if (target is null || !target.Contains(":$task:exit:", StringComparison.Ordinal)) return target;
            var blocks = function.Blocks.Where(block => block.Id == target).ToArray();
            var sites = owners[0].Releases.Where(site => site?.BlockId == target).ToArray();
            if (blocks.Length != 1 || sites.Length == 0
                || blocks[0].Terminator is not { Kind: "branch", TargetBlockId: { } destination }
                || destination.Contains(":$task:exit:", StringComparison.Ordinal)
                || blocks[0].Instructions.Count != 6 * sites.Length
                || sites.Select(site => site.OwnerLocalId).Distinct(StringComparer.Ordinal).Count() != sites.Length
                || sites.Any(site => !ValidRelease(function, site))) return target;
            return destination;
        }
        return function with { Blocks = function.Blocks.Select(block => block with {
            Terminator = block.Terminator with { TargetBlockId = Resolve(block.Terminator.TargetBlockId),
                FalseTargetBlockId = Resolve(block.Terminator.FalseTargetBlockId) } }).ToArray() };
    }

    internal static void Validate(GuestValidationContext context)
    {
        var module = context.Module;
        if (!IsVersion(module))
        {
            if (module.TaskLocalLifetimes is not null || module.Provenance.SemanticSchemaVersion == SemanticSchemaVersion
                || module.Provenance.SemanticVersion == SemanticVersion)
                context.Add(DiagnosticCode, "Task local lifetimes require paired Semantic 45/1.54 and IR 25/1.24.");
            return;
        }
        if (module.Provenance.SemanticSchemaVersion != SemanticSchemaVersion
            || module.Provenance.SemanticVersion != SemanticVersion || module.Language != "csharp"
            || module.TaskLocalLifetimes is not { Functions.Count: > 0 } plan
            || plan.ExceptionModel is not ("none" or "fault" or "exception" or "cleanup" or "cleanup_only" or "cancellation")
            || plan.Functions.Any(function => function is null || function.OwnerLocalIds is not { Count: > 0 and <= 8 }
                || function.Releases is null || function.Releases.Any(site => site is null)
                || function.ScopeExits is null || function.ScopeExits.Any(edge => edge is null))
            || !plan.Functions.Select(function => function.FunctionId).SequenceEqual(plan.Functions
                .Select(function => function.FunctionId).Distinct(StringComparer.Ordinal).OrderBy(id => id, StringComparer.Ordinal)))
        {
            context.Add(DiagnosticCode, "Task local lifetime profile, functions or provenance are missing or noncanonical.");
            return;
        }
        foreach (var owner in plan.Functions)
        {
            var functions = module.Functions.Where(function => function.Id == owner.FunctionId).ToArray();
            if (functions.Length != 1 || !owner.OwnerLocalIds.SequenceEqual(owner.OwnerLocalIds.Distinct(StringComparer.Ordinal)
                    .OrderBy(id => id, StringComparer.Ordinal))
                || owner.OwnerLocalIds.Any(id => functions[0].Locals.Count(local => local.Id == id) != 1))
            {
                context.Add(DiagnosticCode, "Task owner slots must name unique locals of a concrete function.");
                continue;
            }
            var function = functions[0];
            if (owner.Releases.Distinct().Count() != owner.Releases.Count
                || owner.Releases.Any(site => !owner.OwnerLocalIds.Contains(site.OwnerLocalId, StringComparer.Ordinal)
                    || !ValidRelease(function, site)))
                context.Add(DiagnosticCode, "Task retirement must load its owner, perform checked release, and clear the same slot.");
            var actual = function.Blocks.SelectMany(block => block.Instructions.Where(instruction =>
                instruction.Op == "call" && instruction.TargetId == ReleaseFunctionId)
                .Select(instruction => (block.Id, Token: instruction.OperandIds.Count == 1 ? instruction.OperandIds[0] : null))).ToArray();
            if (actual.Length != owner.Releases.Count || actual.Any(call => owner.Releases.Count(site =>
                    site.BlockId == call.Id && site.TokenRegisterId == call.Token) != 1))
                context.Add(DiagnosticCode, "Task release calls and declared retirement sites must correspond exactly.");
            var edges = function.Blocks.Where(block => block.Id.Contains(":$task:exit:", StringComparison.Ordinal))
                .SelectMany(exit => function.Blocks.Where(source => source.Terminator.TargetBlockId == exit.Id
                    || source.Terminator.FalseTargetBlockId == exit.Id).Select(source =>
                        new GuestTaskLocalScopeExit(source.Id, exit.Id, exit.Terminator.TargetBlockId ?? "")))
                .OrderBy(edge => edge.SourceBlockId, StringComparer.Ordinal)
                .ThenBy(edge => edge.ExitBlockId, StringComparer.Ordinal).ToArray();
            var resolved = ResolveScopeExitRoutes(module, function);
            if (!owner.ScopeExits.SequenceEqual(edges)
                || edges.Any(edge => edge.ExitBlockId == edge.TargetBlockId
                    || !function.Blocks.Any(block => block.Id == edge.TargetBlockId)
                    || resolved.Blocks.Any(block => block.Terminator.TargetBlockId == edge.ExitBlockId
                        || block.Terminator.FalseTargetBlockId == edge.ExitBlockId))
                || function.Blocks.Any(block => block.Id.Contains(":$task:exit:", StringComparison.Ordinal)
                    && !edges.Any(edge => edge.ExitBlockId == block.Id)))
                context.Add(DiagnosticCode, "Task scope-exit edges must enter a validated release-only block before the declared successor.");
        }
        if (module.Functions.Any(function => function.Blocks.SelectMany(block => block.Instructions).Any(instruction =>
                instruction.Op == "call" && instruction.TargetId is RetainFunctionId or ReleaseFunctionId or TransferFunctionId)
            && !plan.Functions.Any(owner => owner.FunctionId == function.Id)))
            context.Add(DiagnosticCode, "Every function using Task owner helpers requires a lifetime plan.");
        if (!CheckedGuard(module, ReleaseFunctionId, "3") || !CheckedGuard(module, RetainFunctionId, "2")
            || !CheckedGuard(module, TransferFunctionId, "3", transfer: true))
            context.Add(DiagnosticCode, "Task ownership requires zero-aware retain/release helpers that trap when a nonzero token is rejected.");
        if (HasErrors(module) != (module.LanguageErrorCatalog is not null)
            || HasExceptionFlow(module) != (module.AsyncExceptionRoutes is not null)
            || HasDirectCleanup(module) != (module.DirectAwaitRoutes is not null)
            || HasCancellation(module) != (module.AsyncExceptionTransfers is not null))
            context.Add(DiagnosticCode, "Task lifetime error capabilities disagree with the emitted exception contracts.");
    }

    private static bool ValidRelease(GuestFunction function, GuestTaskLocalReleaseSite site)
    {
        var blocks = function.Blocks.Where(block => block.Id == site.BlockId).ToArray();
        if (blocks.Length != 1) return false;
        var instructions = blocks[0].Instructions;
        int release = Enumerable.Range(0, instructions.Count).FirstOrDefault(index => instructions[index] is
            { Op: "call", TargetId: ReleaseFunctionId, OperandIds.Count: 1 } call
            && call.OperandIds[0] == site.TokenRegisterId, -1);
        if (release < 2 || instructions[release - 1] is not { Op: "convert", OperandIds.Count: 1 } token
            || token.ResultId != site.TokenRegisterId
            || instructions[release - 2] is not { Op: "local_load" } load
            || load.TargetId != site.OwnerLocalId || load.ResultId != token.OperandIds[0]) return false;
        // No operation may observe or overwrite the slot between release and
        // zeroing; the canonical sequence contains only the scalar conversion.
        return release + 3 < instructions.Count
            && instructions[release + 1] is { Op: "constant", Constant.Kind: "int64", Constant.Value: "0" } zero
            && instructions[release + 2] is { Op: "convert", OperandIds.Count: 1 } value
            && value.OperandIds[0] == zero.ResultId && value.ResultId == site.ClearedValueRegisterId
            && instructions[release + 3] is { Op: "local_store", OperandIds.Count: 1 } store
            && store.TargetId == site.OwnerLocalId && store.OperandIds[0] == value.ResultId;
    }

    private static bool CheckedGuard(GuestModule module, string id, string command, bool transfer = false)
    {
        var functions = module.Functions.Where(function => function.Id == id).ToArray();
        if (functions.Length != 1) return false;
        var function = functions[0];
        if (function.ReturnTypeId != "type:void" || function.Parameters.Count != (transfer ? 2 : 1)
            || function.Parameters[0].Id != "token" || function.Parameters[0].TypeId != "type:int64"
            || transfer && (function.Parameters[1].Id != "continuation" || function.Parameters[1].TypeId != "type:int64")
            || function.EntryBlockId != "entry" || function.Blocks.Count != (transfer ? 5 : 4)
            || function.Blocks.Select(block => block.Id).Distinct(StringComparer.Ordinal).Count() != function.Blocks.Count) return false;
        var entry = function.Blocks.SingleOrDefault(block => block.Id == "entry");
        var operate = function.Blocks.SingleOrDefault(block => block.Id == "operate");
        var done = function.Blocks.SingleOrDefault(block => block.Id == "done");
        var failed = function.Blocks.SingleOrDefault(block => block.Id == "failed");
        if (transfer)
        {
            var handoff = function.Blocks.SingleOrDefault(block => block.Id == "transfer");
            if (handoff is not { Instructions.Count: 1, Terminator.Kind: "branch_if",
                    Terminator.ConditionValueId: "transferred", Terminator.TargetBlockId: "operate", Terminator.FalseTargetBlockId: "failed" }
                || handoff.Instructions[0] is not { Op: "call", ResultId: "transferred", TargetId: "import:$async:task_retain_for_continuation_v1" } retain
                || !retain.OperandIds.SequenceEqual(new[] { "token", "continuation" })) return false;
        }
        return entry?.Instructions.Count == 2 && operate?.Instructions.Count == 4
            && entry.Instructions[0] is { Op: "constant", ResultId: "zero64", Constant.Kind: "int64", Constant.Value: "0" }
            && entry.Instructions[1] is { Op: "binary", ResultId: "present", OperatorKind: "not_equals" } present
            && present.OperandIds.SequenceEqual(new[] { "token", "zero64" })
            && entry.Terminator is { Kind: "branch_if", ConditionValueId: "present", FalseTargetBlockId: "done" }
            && entry.Terminator.TargetBlockId == (transfer ? "transfer" : "operate")
            && operate.Instructions[0] is { Op: "constant", ResultId: "command", Constant.Kind: "int32" } number
            && number.Constant!.Value == command
            && operate.Instructions[1] is { Op: "constant", ResultId: "zero32", Constant.Kind: "int32", Constant.Value: "0" }
            && operate.Instructions[2] is { Op: "call", ResultId: "result", TargetId: "import:$async:task_i32_v1" } call
            && call.OperandIds.SequenceEqual(new[] { "command", "token", "zero32", "zero32" })
            && operate.Instructions[3] is { Op: "binary", ResultId: "accepted", OperatorKind: "not_equals" } accepted
            && accepted.OperandIds.SequenceEqual(new[] { "result", "zero64" })
            && operate.Terminator is { Kind: "branch_if", ConditionValueId: "accepted", TargetBlockId: "done", FalseTargetBlockId: "failed" }
            && done is { Instructions.Count: 0, Terminator.Kind: "return" }
            && failed is { Instructions.Count: 0, Terminator.Kind: "trap" };
    }
}
