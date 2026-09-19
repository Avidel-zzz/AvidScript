using Microsoft.CodeAnalysis;

namespace AvidScript.CSharpSemantic;

// One rule for syntax diagnostics and operation projection. Accepted local functions
// use the existing callable/CFG contract after explicit capture normalization.
internal static class SemanticLocalFunctionPolicy
{
    public static bool IsSupported(IMethodSymbol method)
    {
        return method.MethodKind == MethodKind.LocalFunction
            && !method.IsAsync && !method.IsExtern && method.Arity == 0
            && method.ContainingSymbol is IMethodSymbol owner
            && (!owner.IsImplicitlyDeclared || owner.MethodKind == MethodKind.AnonymousFunction);
    }

    public const string DiagnosticCode = "ASCS4010";
    public const string DiagnosticMessage =
        "Local functions currently require synchronous, non-generic declarations with a managed body inside a member; " +
        "async and generic local functions are not yet supported.";
}
