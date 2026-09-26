using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.GuestIr;

public static class GuestAsyncThrowRouteValidator
{
    public const int SchemaVersion = 26;
    public const string IrVersion = "1.25";
    public const int SemanticSchemaVersion = 46;
    public const string SemanticVersion = "1.55";
    private const string TaskImport = "import:$async:task_i32_v1";

    public static bool IsVersion(GuestModule module) =>
        module.SchemaVersion == SchemaVersion && module.IrVersion == IrVersion;

    internal static void Validate(GuestValidationContext context)
    {
        var module = context.Module;
        var raises = module.AsyncExceptionTransfers?.Where(item => item.Kind == "raise_exception" || item.Raise is not null).ToArray()
            ?? Array.Empty<GuestAsyncExceptionTransfer>();
        if (!IsVersion(module))
        {
            if (raises.Length != 0 || module.Provenance.SemanticSchemaVersion == SemanticSchemaVersion
                || module.Provenance.SemanticVersion == SemanticVersion)
                context.Add("ASIR1034", "Routed throws require paired Semantic 46/1.55 and IR 26/1.25.");
            return;
        }
        if (module.Language != "csharp" || module.Provenance.SemanticSchemaVersion != SemanticSchemaVersion
            || module.Provenance.SemanticVersion != SemanticVersion
            || module.TaskLocalLifetimes?.ExceptionModel != "cancellation" || raises.Length == 0
            || raises.Any(item => item.Kind != "raise_exception" || item.Raise is not { } route
                || module.LanguageErrorCatalog?.Types.Any(type => type.Token == route.TypeToken) != true
                || module.LanguageErrorCatalog?.Sources.Any(source => source.Token == route.SourceToken) != true))
            context.Add("ASIR1034", "Routed throws require source/type catalog entries and the cancellation ownership contract.");
    }

    internal static bool Matches(GuestFunction function, GuestAsyncExceptionTransfer transfer, string marker)
    {
        if (transfer.Raise is not { } route) return false;
        var blocks = function.Blocks.GroupBy(block => block.Id, StringComparer.Ordinal)
            .ToDictionary(group => group.Key, group => group.First(), StringComparer.Ordinal);
        if (!blocks.TryGetValue(marker, out var create)
            || !blocks.TryGetValue(marker + ":task_created", out var fault)
            || !blocks.TryGetValue(marker + ":task_create_rejected", out var invalid)
            || !blocks.TryGetValue(marker + ":fault_acquired", out var acquired)
            || !blocks.TryGetValue(marker + ":fault_rejected", out var rejected)
            || !blocks.TryGetValue(marker + ":publish", out var published)
            || invalid.Instructions.Count != 0 || invalid.Terminator.Kind != "trap"
            || rejected.Terminator.Kind != "trap") return false;

        var creates = create.Instructions.Where(item => item.Op == "call").ToArray();
        if (creates.Length != 1 || creates[0] is not { TargetId: TaskImport, OperandIds.Count: 4, ResultId: { } task }
            || create.Instructions.Count != 7
            || !Literal(create, creates[0].OperandIds[0], "int32", 1)
            || !Literal(create, creates[0].OperandIds[1], "int64", 0)
            || !Literal(create, creates[0].OperandIds[2], "int32", 0)
            || !Literal(create, creates[0].OperandIds[3], "int32", 0)
            || !GuestDirectAwaitCancellationValidator.Nonzero(create, task, "int64")
            || !Branches(create, fault.Id, invalid.Id)) return false;
        var calls = fault.Instructions.Where(item => item.Op == "call").ToArray();
        if (calls.Length != 1 || calls[0] is not { TargetId: GuestTaskLanguageErrorValidator.ImportId, OperandIds.Count: 4, ResultId: { } accepted }
            || fault.Instructions.Count != 5 || calls[0].OperandIds[0] != task
            || !Literal(fault, calls[0].OperandIds[1], "int32", route.TypeToken)
            || !Literal(fault, calls[0].OperandIds[2], "int32", route.SourceToken)
            || fault.Terminator.ConditionValueId != accepted
            || !Branches(fault, acquired.Id, rejected.Id)) return false;
        // The common fault import validator independently checks the fresh root,
        // its type field and the catalog tokens before this ownership proof.
        if (!GuestAsyncExceptionTransferValidator.Release(blocks, acquired, transfer, out var cleared)
            || cleared!.Terminator is not { Kind: "branch", TargetBlockId: { } publishTarget }
            || publishTarget != published.Id || published.Instructions.Count != 2
            || !GuestDirectAwaitCancellationValidator.Store(published, transfer.OwnerLocalId, task)
            || !GuestDirectAwaitCancellationValidator.Store(published, transfer.TypeLocalId, calls[0].OperandIds[1])
            || published.Terminator.Kind != "branch" || published.Terminator.TargetBlockId != transfer.TargetBlockId) return false;
        var releases = rejected.Instructions.Where(item => item.Op == "call").ToArray();
        return releases.Length == 1 && releases[0] is { TargetId: TaskImport, OperandIds.Count: 4 }
            && rejected.Instructions.Count == 4
            && releases[0].OperandIds[1] == task
            && Literal(rejected, releases[0].OperandIds[0], "int32", 3)
            && Literal(rejected, releases[0].OperandIds[2], "int32", 0)
            && Literal(rejected, releases[0].OperandIds[3], "int32", 0);
    }

    // A method can catch an explicit throw before a later, unprotected await.
    // Its failure terminates the producer Task instead of entering a handler.
    // Do not infer protection from the mere presence of exception owner locals.
    internal static bool IsTerminalTaskFailure(GuestModule module, GuestFunction function, GuestBasicBlock block)
    {
        if (!IsVersion(module) || block.Terminator.Kind != "return") return false;
        var calls = block.Instructions.Where(item => item.Op == "call"
            && item.TargetId == "import:$async:task_propagate_failure_v1").ToArray();
        if (calls.Length != 1 || calls[0].OperandIds.Count != 2) return false;
        string source = calls[0].OperandIds[0];
        string producer = calls[0].OperandIds[1];
        var producerLoads = block.Instructions.Where(item => item.Op == "local_load" && item.ResultId == producer
            && item.TargetId?.StartsWith("value:local:$async:producer_task:", StringComparison.Ordinal) == true).ToArray();
        if (producerLoads.Length != 1 || !function.Locals.Any(local =>
                local.Id == producerLoads[0].TargetId && local.TypeId == "type:int64")) return false;
        var instructions = function.Blocks.SelectMany(item => item.Instructions).ToArray();
        if (!instructions.Any(item => item.TargetId?.StartsWith("value:local:$async:await_task:", StringComparison.Ordinal) == true
                && (item.Op == "local_load" && item.ResultId == source
                    || item.Op == "local_store" && item.OperandIds.SequenceEqual(new[] { source })))) return false;
        return block.Instructions.All(item => item.Op is "constant" or "local_load" or "convert"
            || item.Op == "local_store" && item.TargetId?.StartsWith("value:local:symbol:local:", StringComparison.Ordinal) == true
            || item.Op == "call" && (item == calls[0]
                || item.TargetId == GuestTaskLocalLifetimeValidator.ReleaseFunctionId
                || item.TargetId == TaskImport && item.OperandIds.Count == 4
                    && Literal(block, item.OperandIds[0], "int32", 3)));
    }

    private static bool Literal(GuestBasicBlock block, string value, string kind, int expected) =>
        GuestDirectAwaitCancellationValidator.Literal(block, value, kind, expected);
    private static bool Branches(GuestBasicBlock block, string yes, string no) =>
        GuestDirectAwaitCancellationValidator.Branches(block, yes, no);
}
