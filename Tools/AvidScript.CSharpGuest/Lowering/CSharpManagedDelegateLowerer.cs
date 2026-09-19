using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal static class CSharpManagedDelegateLowerer
{
    public static GuestRegister? LowerCreation(CSharpFunctionLoweringContext context, SemanticOperation operation,
        int blockOrdinal, List<GuestInstruction> instructions)
    {
        if (CSharpClosureLayout.UsesManagedDelegates(context.Document)) return CSharpClosureDelegateLowerer.Create(context, operation, blockOrdinal, instructions);
        SemanticDelegateType? signature = context.Document.DelegateTypes.FirstOrDefault(item => item.TypeId == operation.TypeId);
        if (!SupportsDelegates(context.Document) || signature is null
            || signature.ReturnRefKind != "none" || operation.Children.Count != 1
            || operation.Children[0] is not { Kind: "method_reference", IsSupported: true, Children.Count: 0 } target
            || !context.TryGetCallTarget(target.SymbolId, out SemanticCallable callable, out string targetId)
            || (callable.MethodSymbolId.Contains(":lambda:", StringComparison.Ordinal)
                && context.Document.SemanticVersion is not "1.25"
                && context.Document.SemanticVersion is not "1.26"
                && context.Document.SemanticVersion != SemanticContract.CurrentSemanticVersion)
            || !callable.IsStatic || callable.IsConstructor || !callable.HasBody || callable.Import is not null
            || callable.ReturnTypeId != signature.ReturnTypeId
            || !callable.Parameters.Select(parameter => (parameter.TypeId, parameter.RefKind))
                .SequenceEqual(signature.Parameters.Select(parameter => (parameter.TypeId, parameter.RefKind))))
        {
            context.Add("ASCG1024", "Delegate creation requires a reachable static Guest method with an exact signature; captured or bound receivers require a managed environment.");
            return null;
        }
        GuestRegister? result = context.CreateTemporary(operation.TypeId, blockOrdinal);
        if (result is not null)
            instructions.Add(new("function_ref", result.Id, Array.Empty<string>(), targetId, null, null));
        return result;
    }

    public static bool TryLowerInvocation(CSharpFunctionLoweringContext context, SemanticOperation operation,
        int blockOrdinal, List<GuestInstruction> instructions, out GuestRegister? result)
    {
        result = null;
        SemanticDelegateType? signature = context.Document.DelegateTypes.FirstOrDefault(item => item.InvokeMethodSymbolId == operation.SymbolId);
        if (signature is null) return false;
        if (!SupportsDelegates(context.Document)
            || signature.ReturnRefKind != "none" || operation.TypeId != signature.ReturnTypeId
            || operation.Children.Count != signature.Parameters.Count + 1
            || operation.Children[0].TypeId != signature.TypeId)
        {
            context.Add("ASCG1024", "Delegate invocation has an unsupported return reference, receiver, or signature.");
            return true;
        }
        GuestRegister? receiver = CSharpOperationLowerer.LowerValue(context, operation.Children[0], blockOrdinal, instructions);
        if (receiver is null) return true;
        SemanticCallableParameter[] parameters = signature.Parameters.Select(parameter => new SemanticCallableParameter(
            parameter.Ordinal, $"{signature.InvokeMethodSymbolId}:parameter:{parameter.Ordinal}", $"arg{parameter.Ordinal}", parameter.TypeId, parameter.RefKind)).ToArray();
        if (receiver is null) return true;
        string[] arguments = new string[parameters.Length];
        // Roslyn retains source evaluation order for named arguments. Evaluate in that
        // order, then place each value/address in the signature's parameter position.
        foreach (SemanticOperation argument in operation.Children.Skip(1))
        {
            int ordinal = Array.FindIndex(parameters, parameter => argument.Kind == "argument"
                && argument.SymbolId?.StartsWith($"symbol:parameter:{signature.InvokeMethodSymbolId}:{parameter.Ordinal}:", StringComparison.Ordinal) == true);
            if (ordinal < 0 || arguments[ordinal] is not null)
            {
                context.Add("ASCG1024", "Delegate argument has no unique bound parameter identity.");
                return true;
            }
            if (!argument.IsSupported || argument.Children.Count != 1 || argument.Children[0].TypeId != parameters[ordinal].TypeId)
            {
                context.Add("ASCG1024", "Delegate argument value or address has an incompatible bound parameter type.");
                return true;
            }
            if (!CSharpCallOperationLowerer.TryLowerArguments(context, new[] { parameters[ordinal] }, new[] { argument },
                blockOrdinal, instructions, out List<string> lowered, CSharpBorrowedReferences.Enabled(context.Document))) return true;
            arguments[ordinal] = lowered[0];
        }
        if (signature.ReturnTypeId != CSharpGuestIds.VoidTypeId)
        {
            result = context.CreateTemporary(signature.ReturnTypeId, blockOrdinal);
            if (result is null) return true;
        }
        if (CSharpClosureLayout.UsesManagedDelegates(context.Document))
        {
            GuestRegister? target = context.CreateTemporary(CSharpClosureLayout.FunctionType(signature.TypeId), blockOrdinal);
            GuestRegister? environment = context.CreateTemporary(CSharpClosureLayout.ObjectType, blockOrdinal);
            if (target is null || environment is null) return true;
            instructions.Add(new("field_load", target.Id, new[] { receiver.Id }, CSharpClosureLayout.TargetField, null, null));
            instructions.Add(new("field_load", environment.Id, new[] { receiver.Id }, CSharpClosureLayout.ContextField, null, null));
            instructions.Add(new("call_indirect", result?.Id, new[] { target.Id, environment.Id }.Concat(arguments).ToArray(), target.TypeId, null, null));
        }
        else instructions.Add(new("call_indirect", result?.Id, new[] { receiver.Id }.Concat(arguments).ToArray(), signature.TypeId, null, null));
        return true;
    }

    public static IReadOnlyList<GuestFunctionReference> BuildContracts(SemanticDocument document,
        IReadOnlyList<GuestType> types, IReadOnlyList<GuestFunction> functions)
    {
        Dictionary<string, SortedSet<string>> targets = types.Where(type => type.Kind == "function_ref")
            .ToDictionary(type => type.Id, _ => new SortedSet<string>(StringComparer.Ordinal), StringComparer.Ordinal);
        foreach (GuestFunction function in functions)
        {
            Dictionary<string, GuestRegister> registers = function.Parameters.Concat(function.Locals).ToDictionary(item => item.Id, StringComparer.Ordinal);
            foreach (GuestInstruction instruction in function.Blocks.SelectMany(block => block.Instructions).Where(item => item.Op == "function_ref"))
                targets[registers[instruction.ResultId!].TypeId].Add(instruction.TargetId!);
        }
        bool closures = CSharpClosureLayout.UsesManagedDelegates(document);
        string FunctionType(SemanticDelegateType signature) => closures ? CSharpClosureLayout.FunctionType(signature.TypeId) : signature.TypeId;
        return document.DelegateTypes.Where(signature => targets.ContainsKey(FunctionType(signature)))
            .OrderBy(signature => signature.TypeId, StringComparer.Ordinal)
            .Select(signature => new GuestFunctionReference(FunctionType(signature),
                (closures ? new[] { CSharpClosureLayout.ObjectType } : Array.Empty<string>())
                    .Concat(signature.Parameters.Select(parameter => CSharpBorrowedReferences.Parameter(document, parameter.TypeId, parameter.RefKind))).ToArray(),
                signature.ReturnTypeId, targets[FunctionType(signature)].ToArray())).ToArray();
    }

    public static bool ContainsReference(string typeId, IReadOnlyDictionary<string, GuestType> types)
        => ContainsReference(typeId, types, new HashSet<string>(StringComparer.Ordinal));

    private static bool SupportsDelegates(SemanticDocument document) => document.SemanticVersion is "1.24" or "1.25" or "1.26"
        || document.SemanticVersion == SemanticContract.CurrentSemanticVersion;

    private static bool ContainsReference(string typeId, IReadOnlyDictionary<string, GuestType> types, HashSet<string> visited)
    {
        if (!visited.Add(typeId) || !types.TryGetValue(typeId, out GuestType? type)) return false;
        return type.Kind is "function_ref" or "managed_ref" || type.Fields.Any(field => ContainsReference(field.TypeId, types, visited))
            || (type.ElementTypeId is { } element && ContainsReference(element, types, visited));
    }
}
