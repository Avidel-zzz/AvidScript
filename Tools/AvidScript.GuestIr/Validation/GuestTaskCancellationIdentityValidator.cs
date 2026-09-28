using System;
using System.Linq;

namespace AvidScript.GuestIr;

internal static class GuestTaskCancellationIdentityValidator
{
    private const string Code = "ASIR1039";

    internal static void Validate(GuestValidationContext context)
    {
        GuestModule artifact = GuestExceptionValues.BaseProfile(context.InputArtifact);
        bool composableAsync = GuestComposableCapabilities.HasDeclaredAsyncBase29(context.InputArtifact);
        var reserved = artifact.Imports.Where(import =>
            import.Id.StartsWith("import:task_cancel_language_error_", StringComparison.Ordinal)
            || import.Name.StartsWith("avid_task_cancel_language_error_", StringComparison.Ordinal)
            || import.Id.StartsWith("import:task_cancellation_token_", StringComparison.Ordinal)
            || import.Name.StartsWith("avid_task_cancellation_token_", StringComparison.Ordinal)).ToArray();
        if (!GuestTaskCancellationIdentity.IsVersion(artifact) && !composableAsync)
        {
            if (artifact.CancellationIdentity is not null || reserved.Any(import =>
                import.Id != GuestTaskCancellationErrorValidator.ImportId || import.Name != GuestTaskCancellationErrorValidator.ImportName))
                Add(context, "Cancellation identity metadata and imports require Guest IR 32/1.31.");
            return;
        }
        if (artifact.CancellationIdentity is not { } plan
            || !GuestTaskCancellationIdentity.IsBase(plan.BaseSchemaVersion, plan.BaseIrVersion)
            || artifact.Language != "csharp"
            || !(GuestTaskCancellationErrorValidator.Supports(context.Module) || composableAsync)
            || artifact.LanguageErrorCatalog is not { Types.Count: > 0, Sources.Count: > 0 }
            || artifact.TaskLocalLifetimes is { ExceptionModel: not "cancellation" }
            || artifact.DirectAwaitReadiness is { } readiness
                && (readiness.BaseSchemaVersion != plan.BaseSchemaVersion || readiness.BaseIrVersion != plan.BaseIrVersion))
        {
            Add(context, "Cancellation identity needs a supported cancellation execution profile and its complete catalog.");
            return;
        }
        var cancel = reserved.Where(import => import.Id == GuestTaskCancellationIdentity.CancelImportId
            || import.Name == GuestTaskCancellationIdentity.CancelImportName).ToArray();
        var read = reserved.Where(import => import.Id == GuestTaskCancellationIdentity.ReadImportId
            || import.Name == GuestTaskCancellationIdentity.ReadImportName).ToArray();
        if (reserved.Length != 2 || cancel.Length != 1 || read.Length != 1
            || !GuestTaskCancellationErrorValidator.IsCancellationImport(cancel[0], true)
            || read[0] is not { Id: GuestTaskCancellationIdentity.ReadImportId, Module: "avidscript",
                Name: GuestTaskCancellationIdentity.ReadImportName, ReturnTypeId: "type:int64",
                DispatchClass: "semantic", OptimizationClass: "none", BindingOrdinal: -1 }
            || !read[0].ParameterTypeIds.SequenceEqual(new[] { "type:int64" }))
        {
            Add(context, "IR 32 requires exactly the v2 cancellation writer and v1 identity reader with canonical signatures.");
            return;
        }
        foreach (var function in context.Module.Functions)
        foreach (var block in function.Blocks)
        for (int index = 0; index < block.Instructions.Count; ++index)
        {
            var instruction = block.Instructions[index];
            if (instruction.Op != "call" || instruction.TargetId != GuestTaskCancellationIdentity.CancelImportId) continue;
            if (!context.CheckedDirectCancellationProducers.Contains((function.Id, block.Id))
                || instruction.OperandIds.Count != 5
                || !HasProof(artifact, function, block, index, instruction.OperandIds[4]))
                Add(context, "Cancellation writer has no checked dispatch or evaluated readiness token: " + function.Id + "/" + block.Id);
        }
        if (artifact.DirectAwaitReadiness is null && artifact.DirectAwaitRoutes is { } routes)
        {
            var scheduleBlocks = routes.Select(route => route.AwaitBlockId).ToHashSet(StringComparer.Ordinal);
            if (artifact.Functions.SelectMany(function => function.Blocks).Where(block => scheduleBlocks.Contains(block.Id))
                .SelectMany(block => block.Instructions).Any(instruction => instruction.Op == "call"
                    && context.Imports.TryGetValue(instruction.TargetId ?? "", out var import)
                    && import is { Module: "env", Name: "continuation_bind_cancel" }))
                Add(context, "IR 32 source-bound awaits require their readiness guard and evaluated token.");
        }
    }

    private static bool HasProof(GuestModule artifact, GuestFunction function, GuestBasicBlock block, int index, string proof)
    {
        var guards = artifact.DirectAwaitReadiness?.Guards.Where(guard => guard.FunctionId == function.Id
            && guard.CancellationBlockId + ":task_created" == block.Id).ToArray();
        if (guards is { Length: > 0 })
        {
            if (guards.Length != 1) return false;
            var checks = function.Blocks.Where(candidate => candidate.Id == guards[0].CheckBlockId)
                .SelectMany(candidate => candidate.Instructions).Where(instruction => instruction.Op == "call"
                    && instruction.TargetId == GuestDirectAwaitReadiness.ImportId).ToArray();
            return checks.Length == 1 && checks[0].OperandIds.SequenceEqual(new[] { proof });
        }
        var definitions = function.Blocks.SelectMany(candidate => candidate.Instructions).ToArray();
        return block.Instructions.Take(index).Any(instruction => instruction.ResultId == proof
                && instruction is { Op: "constant", Constant.Kind: "int64", Constant.Value: "0" })
            && definitions.Count(instruction => instruction.ResultId == proof) == 1
            && definitions.All(instruction => instruction.TargetId != proof);
    }

    private static void Add(GuestValidationContext context, string message) => context.Add(Code, message);
}
