using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;

namespace AvidScript.GuestIr;

// Admission belongs to the actual serialized IR, including its router and
// payload flow. A capability flag cannot authorize a legacy execution view.
internal static class GuestObjectAwaitCancellationValidator
{
    internal static void Validate(GuestValidationContext context)
    {
        var module = context.InputArtifact;
        var imports = module.Imports.Where(import => import.Name == GuestObjectAwaitCancellation.ImportName).ToArray();
        var routes = (module.DirectAwaitRoutes ?? Array.Empty<GuestDirectAwaitRoute>())
            .Where(route => route.ProducerKind == GuestObjectAwaitCancellation.ProducerKind).ToArray();
        if (!GuestObjectAwaitCancellation.IsVersion(module))
        {
            if (module.ObjectAwaitCancellation is not null || imports.Length != 0 || routes.Length != 0)
                Add("Object cancel-resume execution requires paired Semantic 58/1.67 and IR 39/1.38.");
            return;
        }
        if (!GuestObjectAwaitCancellation.HasDeclaredExecutionBase(module)
            || module.ObjectAwaitCancellation is not { } plan
            || !plan.AwaitCallbackIds.SequenceEqual(routes.Select(route => route.CallbackId))
            || plan.AwaitCallbackIds.Any(callback => callback < 0x40000000)
            || !plan.AwaitCallbackIds.SequenceEqual(plan.AwaitCallbackIds.Distinct().Order())
            || imports is not [{ Module: GuestObjectAwaitCancellation.ImportModule,
                DispatchClass: "semantic", OptimizationClass: "none", BindingOrdinal: -1,
                ReturnTypeId: "type:int64" }]
            || !imports[0].ParameterTypeIds.SequenceEqual(new[] { "type:string", "type:int32" })
            || routes.Any(route => route.ScheduleImportId != imports[0].Id))
        { Add("Object await callbacks, execution base and versioned import must agree exactly."); return; }
        if (module.Types.Where(type => type.Id == GuestObjectAwaitCancellation.StatusTypeId).ToArray() is not [
                { Kind: "enum", Storage: "i32", UnderlyingTypeId: "type:int32", Size: 4, Alignment: 4 }]
            || module.Types.Where(type => type.Id == GuestObjectAwaitCancellation.LoadedObjectTypeId).ToArray() is not [
                { Kind: "struct", Storage: "memory", Size: 8, Alignment: 4, Fields: [
                    { Name: "Slot", TypeId: "type:int32", Offset: 0 },
                    { Name: "Generation", TypeId: "type:int32", Offset: 4 }] }])
        { Add("Object continuation payload requires its canonical enum and two-field handle layout."); return; }
        var exports = module.Exports.Where(export => export.Name == "avid_on_continuation_v2").ToArray();
        if (exports.Length != 1 || !context.Functions.TryGetValue(exports[0].FunctionId, out var router)
            || !router.Parameters.Select(parameter => parameter.TypeId).SequenceEqual(new[] {
                "type:int32", "type:int64", "type:int32", "type:int32", "type:int32" })
            || router.ReturnTypeId != "type:void")
        { Add("Object await requires one canonical five-parameter continuation router."); return; }
        foreach (var route in routes)
        {
            string resumeId = "function:synthetic:async_resume:" + route.CallbackId;
            if (!ValidateRouter(module, router, resumeId, route.CallbackId))
                Add("Object callback has no checked Completed/Failed/Cancelled payload dispatch: " + route.CallbackId);
            if (context.Functions.TryGetValue(resumeId, out var resume) && resume.Parameters.Count == 3)
            {
                // The incoming handle cannot be read or published before status
                // selection, or from any cancellation/fault path after selection.
                var blocks = resume.Blocks.GroupBy(block => block.Id).ToDictionary(group => group.Key, group => group.First());
                var normalReach = Reachable(blocks, route.NormalTargetBlockId + ":entry:normal_path");
                var cancelledReach = Reachable(blocks, route.NormalTargetBlockId + ":entry:cancel_path");
                string payload = resume.Parameters[2].Id;
                if (resume.Blocks.Any(block => block.Instructions.Any(instruction => instruction.OperandIds.Contains(payload))
                    && (!normalReach.Contains(block.Id) || cancelledReach.Contains(block.Id))))
                    Add("Object result is used before status admission or on cancellation: " + route.CallbackId);
            }
        }
        var awaitBlocks = routes.Select(route => route.AwaitBlockId).ToHashSet(StringComparer.Ordinal);
        if (module.Functions.SelectMany(function => function.Blocks).Any(block => block.Instructions.Any(instruction =>
                instruction.Op == "call" && instruction.TargetId == imports[0].Id) && !awaitBlocks.Contains(block.Id)))
            Add("Object cancel-resume import has an unlisted scheduling call.");

        void Add(string message) => context.Add("ASIR1044", message);
    }

    private static bool ValidateRouter(GuestModule module, GuestFunction router, string resumeId, int callbackId)
    {
        var blocks = router.Blocks.GroupBy(block => block.Id).ToDictionary(group => group.Key, group => group.First());
        var calls = router.Blocks.Where(block => block.Instructions.Any(instruction => instruction.Op == "call"
            && instruction.TargetId == resumeId)).ToArray();
        if (calls.Length != 1 || calls[0].Instructions.Count != 5) return false;
        var invoke = calls[0];
        var instructions = invoke.Instructions;
        if (instructions[0] is not { Op: "convert", ResultId: { } status }
            || !instructions[0].OperandIds.SequenceEqual(new[] { router.Parameters[2].Id })
            || instructions[1] is not { Op: "stack_alloc", ResultId: { } payload }
            || !router.Locals.Any(local => local.Id == status && local.TypeId == GuestObjectAwaitCancellation.StatusTypeId)
            || !router.Locals.Any(local => local.Id == payload && local.TypeId == GuestObjectAwaitCancellation.LoadedObjectTypeId)
            || instructions[4].Op != "call" || instructions[4].TargetId != resumeId
            || !instructions[4].OperandIds.SequenceEqual(new[] { router.Parameters[1].Id, status, payload })) return false;
        var fields = module.Types.Single(type => type.Id == GuestObjectAwaitCancellation.LoadedObjectTypeId).Fields;
        for (int index = 0; index < 2; index++)
            if (instructions[index + 2].Op != "field_store" || instructions[index + 2].TargetId != fields[index].Id
                || !instructions[index + 2].OperandIds.SequenceEqual(new[] { payload, router.Parameters[index + 3].Id })) return false;
        var completedChecks = router.Blocks.Where(block => IsBranch(block, router.Parameters[2].Id, "1")
            && block.Terminator.TargetBlockId == invoke.Id).ToArray();
        if (completedChecks.Length != 1) return false;
        var completed = completedChecks[0];
        if (!blocks.TryGetValue(completed.Terminator.FalseTargetBlockId ?? "", out var failed)
            || !IsBranch(failed, router.Parameters[2].Id, "2") || failed.Terminator.TargetBlockId != invoke.Id
            || !blocks.TryGetValue(failed.Terminator.FalseTargetBlockId ?? "", out var cancelled)
            || !IsBranch(cancelled, router.Parameters[2].Id, "3") || cancelled.Terminator.TargetBlockId != invoke.Id
            || !blocks.TryGetValue(cancelled.Terminator.FalseTargetBlockId ?? "", out var rejected)
            || rejected.Instructions.Count != 0 || rejected.Terminator.Kind != "trap") return false;
        string literal = callbackId.ToString(CultureInfo.InvariantCulture);
        return router.Blocks.Count(block => IsBranch(block, router.Parameters[0].Id, literal)
            && block.Terminator.TargetBlockId == completed.Id) == 1
            && router.Blocks.SelectMany(block => block.Instructions).All(instruction =>
                !router.Parameters.Any(parameter => parameter.Id == instruction.ResultId || parameter.Id == instruction.TargetId));
    }

    private static bool IsBranch(GuestBasicBlock block, string input, string expected) =>
        block.Terminator.Kind == "branch_if" && block.Instructions.Count == 2
        && block.Instructions[0] is { Op: "constant", ResultId: { } literal, Constant: { Kind: "int32", Value: var value } }
        && value == expected && block.Instructions[1] is { Op: "binary", OperatorKind: "equals", ResultId: { } condition }
        && condition == block.Terminator.ConditionValueId
        && block.Instructions[1].OperandIds.SequenceEqual(new[] { input, literal });

    private static HashSet<string> Reachable(IReadOnlyDictionary<string, GuestBasicBlock> blocks, string entry)
    {
        var reached = new HashSet<string>(StringComparer.Ordinal);
        var pending = new Queue<string>();
        pending.Enqueue(entry);
        while (pending.TryDequeue(out var id))
        {
            if (!reached.Add(id) || !blocks.TryGetValue(id, out var block)) continue;
            if (block.Terminator.TargetBlockId is { } target) pending.Enqueue(target);
            if (block.Terminator.FalseTargetBlockId is { } alternate) pending.Enqueue(alternate);
        }
        return reached;
    }
}
