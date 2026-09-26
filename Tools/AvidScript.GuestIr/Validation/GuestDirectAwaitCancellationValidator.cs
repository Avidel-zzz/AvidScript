using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;

namespace AvidScript.GuestIr;

internal static class GuestDirectAwaitCancellationValidator
{
    internal static bool Validate(GuestModule module, GuestFunction function,
        IReadOnlyDictionary<string, GuestBasicBlock> blocks, string entry,
        GuestDirectAwaitRoute route)
    {
        if (route.Cancellation is not { } cancellation
            || module.LanguageErrorCatalog is not { } catalog
            || !catalog.Types.Any(type => type.Token == cancellation.TypeToken
                && type.TypeId == "type:global::System.Threading.Tasks.TaskCanceledException")
            || !catalog.Sources.Any(source => source.Token == cancellation.SourceToken)
            || !function.Locals.Any(local => local.Id == cancellation.OwnerLocalId && local.TypeId == "type:int64")
            || !function.Locals.Any(local => local.Id == cancellation.TypeLocalId && local.TypeId == "type:int32")
            || !blocks.TryGetValue(entry, out GuestBasicBlock? create)
            || !blocks.TryGetValue(entry + ":task_created", out GuestBasicBlock? cancel)
            || !blocks.TryGetValue(entry + ":cancelled_owner", out GuestBasicBlock? owned)
            || !blocks.TryGetValue(entry + ":cancel_rejected", out GuestBasicBlock? rejected)
            || !blocks.TryGetValue(entry + ":task_create_rejected", out GuestBasicBlock? invalid)
            || invalid.Terminator.Kind != "trap" || rejected.Terminator.Kind != "trap") return false;

        GuestInstruction[] creates = create.Instructions.Where(instruction => instruction.Op == "call"
            && instruction.TargetId == "import:$async:task_i32_v1"
            && instruction.OperandIds.Count == 4
            && Literal(create, instruction.OperandIds[0], "int32", 1)).ToArray();
        if (creates.Length != 1 || creates[0] is not { OperandIds.Count: 4, ResultId: { } task }
            || !Literal(create, creates[0].OperandIds[0], "int32", 1)
            || !Literal(create, creates[0].OperandIds[1], "int64", 0)
            || !Literal(create, creates[0].OperandIds[2], "int32", 0)
            || !Literal(create, creates[0].OperandIds[3], "int32", 0)
            || !Nonzero(create, task, "int64")
            || !Branches(create, cancel.Id, invalid.Id)) return false;
        GuestInstruction[] calls = cancel.Instructions.Where(instruction => instruction.Op == "call"
            && instruction.TargetId == GuestTaskCancellationErrorValidator.ImportId).ToArray();
        if (calls.Length != 1 || calls[0] is not { OperandIds.Count: 4, ResultId: { } accepted }
            || calls[0].OperandIds[0] != task
            || !Literal(cancel, calls[0].OperandIds[1], "int32", cancellation.TypeToken)
            || !Literal(cancel, calls[0].OperandIds[2], "int32", cancellation.SourceToken)
            || cancel.Terminator.ConditionValueId != accepted
            || !Branches(cancel, owned.Id, rejected.Id)
            || owned.Terminator.Kind != "branch"
            || owned.Terminator.TargetBlockId != route.CancellationTargetBlockId
            || !Store(owned, cancellation.OwnerLocalId, task)
            || !Store(owned, cancellation.TypeLocalId, calls[0].OperandIds[1])) return false;
        GuestInstruction[] releases = rejected.Instructions.Where(instruction => instruction.Op == "call"
            && instruction.TargetId == "import:$async:task_i32_v1").ToArray();
        return releases.Length == 1 && releases[0].OperandIds.Count == 4
            && Literal(rejected, releases[0].OperandIds[0], "int32", 3)
            && releases[0].OperandIds[1] == task;
    }

    internal static bool Literal(GuestBasicBlock block, string value, string kind, int expected) =>
        block.Instructions.Count(instruction => instruction.ResultId == value) == 1
        && block.Instructions.Any(instruction => instruction.Op == "constant"
            && instruction.ResultId == value && instruction.Constant?.Kind == kind
            && instruction.Constant.Value == expected.ToString(CultureInfo.InvariantCulture));

    internal static bool Nonzero(GuestBasicBlock block, string value, string kind) =>
        block.Instructions.Any(instruction => instruction.Op == "binary"
            && instruction.ResultId == block.Terminator.ConditionValueId
            && instruction.OperatorKind == "not_equals" && instruction.OperandIds.Count == 2
            && instruction.OperandIds[0] == value && Literal(block, instruction.OperandIds[1], kind, 0));

    internal static bool Branches(GuestBasicBlock block, string yes, string no) =>
        block.Terminator.Kind == "branch_if" && block.Terminator.TargetBlockId == yes
        && block.Terminator.FalseTargetBlockId == no;

    internal static bool Store(GuestBasicBlock block, string local, string value) =>
        block.Instructions.Count(instruction => instruction.Op == "local_store" && instruction.TargetId == local) == 1
        && block.Instructions.Any(instruction => instruction.Op == "local_store"
            && instruction.TargetId == local && instruction.OperandIds.SequenceEqual(new[] { value }));
}
