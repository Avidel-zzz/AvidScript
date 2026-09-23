using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal static class CSharpReferenceObjectCreation
{
    public static GuestRegister? Lower(CSharpFunctionLoweringContext context, SemanticOperation operation, int block, List<GuestInstruction> instructions)
    {
        SemanticCallable? constructor = context.Document.Callables.SingleOrDefault(callable => callable.MethodSymbolId == operation.SymbolId);
        if (constructor is null || !constructor.IsConstructor || constructor.IsStatic
            || (constructor.ContainingTypeId != operation.TypeId
                && !CSharpReferenceObjects.IsClosedImplicitConstructor(
                    context.Document, operation.TypeId, constructor))
            || constructor.ReturnTypeId != CSharpGuestIds.VoidTypeId
            || constructor.Import is not null || (!constructor.HasBody && (!context.Document.ClassTypes.Single(type => type.TypeId == operation.TypeId).HasImplicitDefaultConstructor
                || constructor.Parameters.Count != 0)))
        { context.Add("ASCG1024", "Reference creation requires an exact source constructor or its declared implicit default constructor."); return null; }
        if (!CSharpCallOperationLowerer.TryLowerArguments(context, constructor.Parameters, operation.Children, block, instructions,
                out List<string> arguments, borrowed: true)) return null;
        GuestRegister? instance = context.CreateTemporary(operation.TypeId, block);
        if (instance is null) return null;
        instructions.Add(new("managed_new", instance.Id, Array.Empty<string>(), null, null, null));
        if (constructor.HasBody)
        {
            if (!context.TryGetCallTarget(constructor.MethodSymbolId, out _, out string target))
            { context.Add("ASCG1024", "Reference constructor must be reachable."); return null; }
            arguments.Insert(0, instance.Id);
            CSharpOperationLowerer.EmitCall(context, constructor, target, arguments, block, instructions);
        }
        return instance;
    }

    public static bool IsRootInitializer(CSharpFunctionLoweringContext context, SemanticOperation operation) =>
        context.Callable.IsConstructor && CSharpReferenceObjects.Types(context.Document).Contains(context.Callable.ContainingTypeId)
        && operation.TypeId == CSharpGuestIds.VoidTypeId
        && operation.Children.Count == 1 && operation.Children[0].Kind == "instance_reference"
        && context.Document.Callables.Any(callable => callable.MethodSymbolId == operation.SymbolId && callable.ContainingTypeId == "type:object"
            && callable.IsConstructor && !callable.IsStatic && !callable.HasBody && callable.Import is null && callable.Parameters.Count == 0
            && callable.ReturnTypeId == CSharpGuestIds.VoidTypeId);
}
