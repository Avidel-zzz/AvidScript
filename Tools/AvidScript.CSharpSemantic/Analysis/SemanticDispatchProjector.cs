using System;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.FlowAnalysis;
using Microsoft.CodeAnalysis.Operations;

namespace AvidScript.CSharpSemantic;

internal static class SemanticDispatchProjector
{
    public static SemanticCallableDispatch Project(IMethodSymbol method) => new(
        method.IsVirtual, method.IsAbstract, method.IsOverride, method.IsSealed,
        method.OverriddenMethod is { } parent ? SemanticSymbolProjector.GetSymbolId(parent) : null,
        Slot(method),
        method.ExplicitInterfaceImplementations.Select(SemanticSymbolProjector.GetSymbolId)
            .Distinct(StringComparer.Ordinal).OrderBy(id => id, StringComparer.Ordinal).ToArray());

    public static SemanticMethodDispatch? Project(IOperation operation)
    {
        (IMethodSymbol? method, bool isVirtual, IOperation? receiver) = operation switch
        {
            IInvocationOperation call => (call.TargetMethod, call.IsVirtual, call.Instance),
            IMethodReferenceOperation reference => (reference.Method, reference.IsVirtual, reference.Instance),
            IAnonymousFunctionOperation lambda => (lambda.Symbol, false, null),
            IFlowAnonymousFunctionOperation lambda => (lambda.Symbol, false, null),
            _ => (null, false, null),
        };
        if (method is null) return null;
        bool isBase = receiver?.Syntax is BaseExpressionSyntax;
        // Roslyn marks a base method-reference as virtual because of its target declaration.
        // C# base access still binds that implementation directly when creating the delegate.
        string kind = isBase ? "direct" : method.MethodKind == MethodKind.DelegateInvoke ? "delegate"
            : method.ContainingType.TypeKind == TypeKind.Interface && !isBase ? "interface"
            : method.IsStatic ? "static" : isVirtual ? "virtual" : "direct";
        return new SemanticMethodDispatch(kind, Slot(method), isBase);
    }

    private static string? Slot(IMethodSymbol method)
    {
        if (!method.IsVirtual && !method.IsAbstract && !method.IsOverride) return null;
        while (method.OverriddenMethod is { } parent) method = parent;
        return SemanticSymbolProjector.GetSymbolId(method);
    }
}
