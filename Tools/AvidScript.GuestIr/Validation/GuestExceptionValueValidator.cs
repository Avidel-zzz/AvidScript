using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.GuestIr;

internal static class GuestExceptionValueValidator
{
    internal static void Validate(GuestValidationContext context)
    {
        var artifact = GuestCancellationTokens.BaseProfile(context.InputArtifact);
        bool composableAsync = GuestComposableCapabilities.HasDeclaredAsyncBase29(context.InputArtifact);
        bool reserved = artifact.Functions.SelectMany(function => function.Locals)
            .Any(local => local.Id.StartsWith(GuestExceptionValues.CapturePrefix, StringComparison.Ordinal));
        if (GuestAsyncVoidErrorOwners.IsVersion(artifact) && artifact.ExceptionValues is null && !reserved) return;
        if (!GuestExceptionValues.IsVersion(artifact) && !composableAsync)
        {
            if (artifact.ExceptionValues is not null || reserved
                || artifact.Provenance.SemanticSchemaVersion == GuestExceptionValues.SemanticSchemaVersion
                || artifact.Provenance.SemanticVersion == GuestExceptionValues.SemanticVersion)
                Add("Exception values require paired Semantic 52/1.61 and Guest IR 33/1.32.");
            return;
        }
        if (artifact.Language != "csharp" || artifact.ExceptionValues?.Bindings is not { Count: > 0 and <= 4096 } bindings
            || artifact.Provenance.SemanticSchemaVersion != (composableAsync ? GuestComposableCapabilities.ExpectedSemanticSchema(artifact) : GuestExceptionValues.SemanticSchemaVersion)
            || artifact.Provenance.SemanticVersion != (composableAsync ? GuestComposableCapabilities.ExpectedSemanticVersion(artifact) : GuestExceptionValues.SemanticVersion)
            || artifact.CancellationIdentity is not { BaseSchemaVersion: 29, BaseIrVersion: "1.28" }
            || !composableAsync && artifact.StaticStorage is not null
            || artifact.AsyncSynchronousExceptions is null)
        {
            Add("Exception values require their source contract, bindings and cancellation-aware synchronous async execution profile.");
            return;
        }
        HashSet<(string Function, string Block)> seen = new();
        HashSet<(string Function, string Register)> roots = new();
        (string Function, string Block)? previous = null;
        foreach (var binding in bindings)
        {
            if (binding is null || !context.Functions.TryGetValue(binding.FunctionId ?? "", out var function)
                || !context.Functions.ContainsKey(binding.MethodFunctionId ?? "")
                || !binding.MethodFunctionId!.StartsWith("function:", StringComparison.Ordinal)
                || binding.FunctionId != binding.MethodFunctionId
                    && !binding.FunctionId!.StartsWith("function:synthetic:async_resume:", StringComparison.Ordinal)
                || binding.OwnerLocalId != "value:local:$async:exception_source:" + binding.MethodFunctionId[9..]
                || !function.Locals.Any(local => local.Id == binding.OwnerLocalId && local.TypeId == "type:int64")
                || !function.Locals.Any(local => local.Id == binding.VariableLocalId && local.TypeId == binding.ReferenceTypeId)
                || !context.Types.TryGetValue(binding.ReferenceTypeId ?? "", out var view)
                || view is not { Kind: "managed_ref", ElementTypeId: null, Size: 8, Alignment: 8 }
                || !seen.Add((function.Id, binding.BlockId)))
            { Add("Malformed exception-value binding."); continue; }
            if (previous is { } before && (string.CompareOrdinal(before.Function, function.Id) > 0
                || before.Function == function.Id && string.CompareOrdinal(before.Block, binding.BlockId) >= 0))
                Add("Exception-value bindings must be ordered and unique.");
            previous = (function.Id, binding.BlockId);
            var blocks = function.Blocks.Where(block => block.Id == binding.BlockId).ToArray();
            if (blocks.Length != 1 || blocks[0].Instructions.Count != 4
                || function.EntryBlockId == binding.BlockId || blocks[0].Terminator.Kind != "branch"
                || blocks[0].Terminator.TargetBlockId == binding.BlockId)
            { Add("Exception binding requires one complete root acquisition block."); continue; }
            var code = blocks[0].Instructions;
            if (code[0] is not { Op: "local_load", ResultId: { } task, OperandIds.Count: 0 }
                || code[0].TargetId != binding.OwnerLocalId
                || !function.Locals.Any(local => local.Id == task && local.TypeId == "type:int64")
                || code[1] is not { Op: "call", ResultId: { } root, TargetId: GuestExceptionValues.RootImportId }
                || !code[1].OperandIds.SequenceEqual(new[] { task })
                || code[2] is not { Op: "managed_cast", ResultId: { } reference, TargetId: null }
                || !code[2].OperandIds.SequenceEqual(new[] { root })
                || !root.StartsWith(GuestExceptionValues.CapturePrefix, StringComparison.Ordinal)
                || !function.Locals.Any(local => local.Id == root && local.TypeId == "type:language_error_root")
                || !function.Locals.Any(local => local.Id == reference && local.TypeId == binding.ReferenceTypeId)
                || code[3].Op != "local_store" || code[3].ResultId is not null || code[3].TargetId != binding.VariableLocalId
                || !code[3].OperandIds.SequenceEqual(new[] { reference })
                || !roots.Add((function.Id, root))) { Add("Exception value must alias the checked Task error root before the owner is released."); continue; }
            var instructions = function.Blocks.SelectMany(block => block.Instructions).ToArray();
            if (instructions.Count(instruction => instruction.ResultId == root) != 1
                || instructions.Count(instruction => instruction.OperandIds.Contains(root)) != 1
                || instructions.Any(instruction => instruction.TargetId == root))
                Add("Captured root register must have one definition and one reference conversion.");
            string typeSlot = "value:local:$async:exception_type:" + binding.MethodFunctionId[9..];
            foreach (var predecessor in function.Blocks.Where(block => block.Terminator.TargetBlockId == binding.BlockId
                || block.Terminator.FalseTargetBlockId == binding.BlockId))
            {
                var match = predecessor.Instructions.LastOrDefault();
                if (predecessor.Terminator.Kind != "branch_if" || predecessor.Terminator.TargetBlockId != binding.BlockId
                    || predecessor.Terminator.FalseTargetBlockId == binding.BlockId
                    || match is not { Op: "binary", OperatorKind: "equals", OperandIds.Count: 2 }
                    || match.ResultId != predecessor.Terminator.ConditionValueId)
                { Add("Catch value can only be entered from a successful exception-type match."); continue; }
                var actual = instructions.Where(instruction => instruction.ResultId == match.OperandIds[0]).ToArray();
                var expected = instructions.Where(instruction => instruction.ResultId == match.OperandIds[1]).ToArray();
                if (actual is not [{ Op: "local_load" }] || actual[0].TargetId != typeSlot
                    || expected is not [{ Op: "constant", Constant.Kind: "int32" }]
                    || !int.TryParse(expected[0].Constant!.Value, out int token)
                    || artifact.LanguageErrorCatalog?.Types.Any(type => type.Token == token) != true)
                    Add("Catch dispatch must match its owner's checked language-error type.");
            }
        }
        foreach (var function in artifact.Functions)
        foreach (var local in function.Locals.Where(local => local.Id.StartsWith(GuestExceptionValues.CapturePrefix, StringComparison.Ordinal)))
            if (!roots.Contains((function.Id, local.Id))) Add("Unlisted exception-value capture register.");

        void Add(string message) => context.Add("ASIR1040", message);
    }
}
