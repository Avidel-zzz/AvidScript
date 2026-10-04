using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

// Catch values alias the original managed error root. Local/frame stores use
// normal managed-reference rooting; the Task remains only the transfer owner.
internal static class CSharpAsyncCatchValues
{
    internal static IEnumerable<GuestType> ReferenceTypes(SemanticDocument source)
    {
        if (!SemanticContract.HasAsyncCatchVariables(source)
            || CSharpAsyncSynchronousExecutionContext.Find(source) is not { } context) yield break;
        var ids = context.GuestCatalog.Types.Select(type => type.TypeId)
            .Concat(source.AsyncMethods.SelectMany(method => method.ExceptionPlan?.Catches
                .Where(handler => handler.ExceptionTypeId is not null).Select(handler => handler.ExceptionTypeId!)
                ?? Array.Empty<string>()))
            .Append(CSharpThrowProducerLowerer.ExceptionTypeId).Append("type:object")
            .Concat(SemanticContract.HasCancellationTokens(source) ? source.Methods
                .SelectMany(method => CSharpCancellationTokenExecutionContext.Operations(method.Root))
                .Where(operation => operation.Kind == SemanticCancellationTokens.Read)
                .Select(operation => operation.Children[0].TypeId!) : Array.Empty<string>())
            .ToHashSet(StringComparer.Ordinal);
        var classes = source.ClassTypes.ToDictionary(type => type.TypeId, StringComparer.Ordinal);
        var pending = new Queue<string>(ids);
        while (pending.TryDequeue(out var id))
            if (classes.TryGetValue(id, out var shape) && shape.BaseTypeId is { } parent && ids.Add(parent)) pending.Enqueue(parent);
        foreach (var type in source.Types.Where(type => ids.Contains(type.Id)).OrderBy(type => type.Id, StringComparer.Ordinal))
            yield return new(type.Id, "managed_ref", "i64", Array.Empty<GuestField>(), null, null, 8, 8);
    }

    internal static GuestRegister? Read(CSharpFunctionLoweringContext context, SemanticAsyncMethod method,
        SemanticOperation operation, int block, List<GuestInstruction> instructions)
    {
        if (!SemanticContract.HasAsyncCatchVariables(context.Document)
            || CSharpAsyncSynchronousExecutionContext.Find(context.Document) is null) return null;
        var owner = CSharpAsyncExceptionLowerer.LoadOwner(context, method, block, instructions);
        var root = context.CreateTemporary("type:language_error_root", block);
        var reference = context.CreateTemporary(operation.TypeId, block);
        if (owner is null || root is null || reference is null) return null;
        instructions.Add(new("call", root.Id, new[] { owner.Id }, GuestExceptionValues.RootImportId, null, null));
        instructions.Add(new("managed_cast", reference.Id, new[] { root.Id }, null, null, null));
        return reference;
    }

    internal static bool TryWrap(SemanticDocument source, GuestModule input,
        out GuestModule module, out string? error, CSharpAsyncSynchronousExecutionContext? asyncContext = null)
    {
        module = input;
        error = null;
        // Existing execution validators check the completed base implementation.
        // This private view never escapes: publication keeps the source contract.
        var objectContext = CSharpObjectAwaitExecutionContext.Find(source);
        var execution = objectContext is not null ? input : input with { Provenance = input.Provenance with {
            SemanticSchemaVersion = GuestAsyncSynchronousExceptions.SemanticSchemaVersion,
            SemanticVersion = GuestAsyncSynchronousExceptions.SemanticVersion } };
        var tokenContext = CSharpCancellationTokenExecutionContext.Find(source);
        var staticContext = CSharpStaticExecutionContext.Find(source);
        GuestModule? upgraded;
        if (!(objectContext is not null
            ? CSharpCancellationIdentityCompiler.TryUpgradeForObjectAwait(objectContext, execution, out upgraded, out error)
            : asyncContext is { HasVoidErrorOwners: true }
            ? CSharpCancellationIdentityCompiler.TryUpgradeForVoidOwners(asyncContext, execution, out upgraded, out error)
            : staticContext is not null && tokenContext is null
            ? CSharpCancellationIdentityCompiler.TryUpgradeForStaticAsyncValue(staticContext, execution, out upgraded, out error)
            : tokenContext is null
            ? CSharpCancellationIdentityCompiler.TryUpgrade(execution, out upgraded, out error)
            : CSharpCancellationIdentityCompiler.TryUpgradeForTokens(tokenContext, execution, out upgraded, out error))) return false;
        var sources = source.AsyncMethods.SelectMany(method => method.Segments
            .Where(segment => segment.Statements.Count == 1
                && segment.Statements[0].Operation.Kind == SemanticAsyncCatchVariableValidator.BindingOperationKind)
            .Select(segment => new {
                Method = CSharpGuestIds.Function(method.MethodSymbolId),
                Block = CSharpGuestIds.AsyncSegmentBlock(method.MethodSymbolId, segment.Ordinal),
                Owner = CSharpGuestIds.Local(CSharpTaskResultAbi.ExceptionSourceSlot(method)),
                Variable = CSharpGuestIds.Local(segment.Statements[0].TargetSymbolId!),
                Type = segment.Statements[0].Operation.TypeId!,
            })).ToDictionary(item => item.Block, StringComparer.Ordinal);
        var bindings = new List<GuestExceptionValueBinding>();
        var functions = new List<GuestFunction>();
        foreach (var function in upgraded!.Functions)
        {
            var rename = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (var block in function.Blocks)
            {
                if (!sources.TryGetValue(block.Id, out var binding)) continue;
                if (block.Instructions.Count != 4 || block.Instructions[1] is not {
                    Op: "call", TargetId: GuestExceptionValues.RootImportId, ResultId: { } root })
                {
                    error = "Catch binding lost its complete root acquisition block: " + block.Id;
                    return false;
                }
                rename.Add(root, GuestExceptionValues.CapturePrefix + root);
                bindings.Add(new(binding.Method, function.Id, block.Id, binding.Owner, binding.Variable, binding.Type));
            }
            string Name(string name) => rename.GetValueOrDefault(name, name);
            functions.Add(function with {
                Locals = function.Locals.Select(local => local with { Id = Name(local.Id) }).ToArray(),
                Blocks = function.Blocks.Select(block => block with { Instructions = block.Instructions.Select(instruction => instruction with {
                    ResultId = instruction.ResultId is { } result ? Name(result) : null,
                    OperandIds = instruction.OperandIds.Select(Name).ToArray(),
                }).ToArray() }).ToArray(),
            });
        }
        module = upgraded with {
            SchemaVersion = GuestExceptionValues.SchemaVersion,
            IrVersion = GuestExceptionValues.IrVersion,
            Provenance = input.Provenance,
            Functions = functions,
            ExceptionValues = bindings.Count == 0 && (objectContext is not null || tokenContext is not null || asyncContext is { HasVoidErrorOwners: true }) ? null
                : new(bindings.OrderBy(binding => binding.FunctionId, StringComparer.Ordinal)
                    .ThenBy(binding => binding.BlockId, StringComparer.Ordinal).ToArray()),
        };
        return true;
    }
}
