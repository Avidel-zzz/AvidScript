using Microsoft.CodeAnalysis;

namespace AvidScript.CSharpSemantic;

internal static class SemanticLambdaPolicy
{
    public static bool IsSupported(IMethodSymbol method) => method.MethodKind == MethodKind.AnonymousFunction
        && !method.IsAsync && method.ContainingSymbol is IMethodSymbol;

    public const string DiagnosticCode = "ASCS4001";
    public const string DiagnosticMessage = "Lambdas currently require a synchronous body inside an executable member; async lambdas and initializer closures are not yet supported.";
}
