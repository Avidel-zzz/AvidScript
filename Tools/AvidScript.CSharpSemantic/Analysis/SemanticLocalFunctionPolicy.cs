using Microsoft.CodeAnalysis;

namespace AvidScript.CSharpSemantic;

// One rule for syntax diagnostics and operation projection. Accepted local functions
// use the existing callable/CFG contract and never capture an implicit receiver.
internal static class SemanticLocalFunctionPolicy
{
    public static bool IsSupported(IMethodSymbol method)
    {
        return method.MethodKind == MethodKind.LocalFunction
            && method.IsStatic && !method.IsAsync && !method.IsExtern && method.Arity == 0
            && method.ContainingSymbol is IMethodSymbol { IsImplicitlyDeclared: false };
    }

    public const string DiagnosticCode = "ASCS4010";
    public const string DiagnosticMessage =
        "Local functions currently require static, synchronous, non-generic declarations with a managed body inside a member; " +
        "captured environments, async local functions, and generic local functions are not yet supported.";
}
