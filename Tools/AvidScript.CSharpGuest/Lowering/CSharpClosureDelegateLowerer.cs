using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal static class CSharpClosureDelegateLowerer
{
    public static GuestRegister? Create(CSharpFunctionLoweringContext context, SemanticOperation operation, int block, List<GuestInstruction> instructions)
    {
        SemanticDelegateType? signature = context.Document.DelegateTypes.SingleOrDefault(item => item.TypeId == operation.TypeId);
        if (signature is null || operation.Children.Count != 1
            || operation.Children[0] is not { Kind: "method_reference", Children.Count: 0 } target)
        { context.Add("ASCG1024", "Closure delegate requires a nominal signature and an unbound Guest method reference."); return null; }
        if (!context.TryGetCallTarget(target.SymbolId, out SemanticCallable callable, out _))
        { context.Add("ASCG1024", $"Closure delegate target '{target.SymbolId}' is not a reachable Guest callable."); return null; }
        if (!Matches(context.Document, callable, signature))
        { context.Add("ASCG1024", $"Closure delegate target '{target.SymbolId}' does not match '{signature.TypeId}' after environment parameters are removed."); return null; }
        GuestRegister erased = context.CreateTemporary(CSharpClosureLayout.ObjectType, block)!;
        IReadOnlyList<SemanticClosureEnvironment> environments = CSharpClosureLayout.Environments(context.Document, callable.MethodSymbolId);
        if (environments.Count == 0) instructions.Add(new("constant", erased.Id, Array.Empty<string>(), null, null, new("null", null)));
        else
        {
            GuestRegister binding = context.CreateTemporary(CSharpClosureLayout.Reference(CSharpClosureLayout.Binding(callable.MethodSymbolId)), block)!;
            instructions.Add(new("managed_new", binding.Id, Array.Empty<string>(), null, null, null));
            foreach (SemanticClosureEnvironment environment in environments)
            {
                GuestRegister? source = context.ClosureCells.Environment(environment.Id);
                if (source is null) { context.Add("ASCG1024", "Closure construction cannot resolve a shared lexical environment."); return null; }
                instructions.Add(new("managed_set", null, new[] { binding.Id, source.Id }, environment.Id, null, null));
            }
            instructions.Add(new("managed_cast", erased.Id, new[] { binding.Id }, null, null, null));
        }
        GuestRegister function = context.CreateTemporary(CSharpClosureLayout.FunctionType(signature.TypeId), block)!;
        instructions.Add(new("function_ref", function.Id, Array.Empty<string>(), CSharpClosureLayout.Thunk(callable.MethodSymbolId, signature.TypeId), null, null));
        GuestRegister result = context.CreateTemporary(signature.TypeId, block)!;
        instructions.Add(new("constant", result.Id, Array.Empty<string>(), null, null, new("zero", null)));
        instructions.Add(new("field_store", null, new[] { result.Id, function.Id }, CSharpClosureLayout.TargetField, null, null));
        instructions.Add(new("field_store", null, new[] { result.Id, erased.Id }, CSharpClosureLayout.ContextField, null, null));
        return result;
    }

    public static IReadOnlyList<GuestFunction> BuildThunks(SemanticDocument document, IReadOnlyList<GuestFunction> functions)
    {
        if (!CSharpClosureLayout.UsesManagedDelegates(document)) return Array.Empty<GuestFunction>();
        HashSet<string> targets = functions.SelectMany(function => function.Blocks).SelectMany(block => block.Instructions)
            .Where(instruction => instruction.Op == "function_ref").Select(instruction => instruction.TargetId!).ToHashSet(StringComparer.Ordinal);
        List<GuestFunction> thunks = new();
        foreach (SemanticDelegateType signature in document.DelegateTypes)
            foreach (SemanticCallable callable in document.Callables.Where(callable => Matches(document, callable, signature)))
            {
                string id = CSharpClosureLayout.Thunk(callable.MethodSymbolId, signature.TypeId);
                if (!targets.Contains(id)) continue;
                List<GuestRegister> parameters = new() { new("context", CSharpClosureLayout.ObjectType) };
                parameters.AddRange(signature.Parameters.Select(parameter => new GuestRegister("argument:" + parameter.Ordinal,
                    CSharpBorrowedReferences.Parameter(document, parameter.TypeId, parameter.RefKind))));
                List<GuestRegister> locals = new(); List<GuestInstruction> instructions = new();
                Dictionary<string, string> environmentValues = new(StringComparer.Ordinal);
                IReadOnlyList<SemanticClosureEnvironment> environments = CSharpClosureLayout.Environments(document, callable.MethodSymbolId);
                if (environments.Count > 0)
                {
                    locals.Add(new("binding", CSharpClosureLayout.Reference(CSharpClosureLayout.Binding(callable.MethodSymbolId))));
                    instructions.Add(new("managed_cast", "binding", new[] { "context" }, null, null, null));
                    foreach (SemanticClosureEnvironment environment in environments)
                    {
                        string local = "environment:" + environmentValues.Count;
                        environmentValues.Add(environment.Id, local); locals.Add(new(local, CSharpClosureLayout.Reference(environment.Id)));
                        instructions.Add(new("managed_get", local, new[] { "binding" }, environment.Id, null, null));
                    }
                }
                List<string> arguments = parameters.Skip(1).Select(parameter => parameter.Id).ToList();
                foreach (SemanticCallableParameter parameter in callable.Parameters)
                    if (CSharpClosureLayout.CapturedParameter(document, parameter.SymbolId) is { } capture)
                        arguments.Add(environmentValues[capture.Environment.Id]);
                string? returned = signature.ReturnTypeId == CSharpGuestIds.VoidTypeId ? null : "returned";
                if (returned is not null) locals.Add(new(returned, signature.ReturnTypeId));
                instructions.Add(new("call", returned, arguments, CSharpGuestIds.Function(callable.MethodSymbolId), null, null));
                thunks.Add(new(id, parameters, locals, signature.ReturnTypeId, "entry", new[] { new GuestBasicBlock("entry", instructions, new("return", null, null, null, returned)) }));
            }
        return thunks;
    }

    private static bool Matches(SemanticDocument document, SemanticCallable callable, SemanticDelegateType signature)
        => signature.ReturnRefKind == "none" && callable.IsStatic && !callable.IsConstructor && callable.HasBody && callable.Import is null
            && callable.ReturnTypeId == signature.ReturnTypeId
            && callable.Parameters.Where(parameter => CSharpClosureLayout.CapturedParameter(document, parameter.SymbolId) is null)
                .Select(parameter => (parameter.TypeId, parameter.RefKind)).SequenceEqual(signature.Parameters.Select(parameter => (parameter.TypeId, parameter.RefKind)));
}
