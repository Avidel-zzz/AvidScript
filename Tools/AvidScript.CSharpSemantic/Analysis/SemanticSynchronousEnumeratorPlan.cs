using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace AvidScript.CSharpSemantic;

// Restrict the first non-array foreach path to sealed source classes. This makes
// each implicit method call an exact Guest target instead of guessing interface
// or virtual dispatch, and requires a concrete Dispose implementation.
internal sealed record SemanticSynchronousEnumeratorPlan(
    INamedTypeSymbol CollectionType,
    INamedTypeSymbol EnumeratorType,
    IMethodSymbol GetEnumerator,
    IMethodSymbol MoveNext,
    IPropertySymbol Current,
    IMethodSymbol Dispose,
    ILocalSymbol Item);

internal static class SemanticSynchronousEnumeratorPlanner
{
    public static bool TryCreate(
        ForEachStatementSyntax loop,
        SemanticModel semanticModel,
        out SemanticSynchronousEnumeratorPlan? plan)
    {
        plan = null;
        if (loop.AwaitKeyword.RawKind != 0
            || loop.Type is RefTypeSyntax
            || semanticModel.GetTypeInfo(loop.Expression).Type is not INamedTypeSymbol
            { TypeKind: TypeKind.Class, IsSealed: true } collection
            || collection.DeclaringSyntaxReferences.Length == 0
            || semanticModel.GetDeclaredSymbol(loop) is not ILocalSymbol item)
        {
            return false;
        }

        ForEachStatementInfo info = semanticModel.GetForEachStatementInfo(loop);
        if (info.GetEnumeratorMethod is not { } getEnumerator
            || getEnumerator.ReturnType is not INamedTypeSymbol
                { TypeKind: TypeKind.Class, IsSealed: true } enumerator
            || enumerator.DeclaringSyntaxReferences.Length == 0
            || info.MoveNextMethod is not { } moveNext
            || info.CurrentProperty is not { } current
            || current.GetMethod is not { } getter
            || info.DisposeMethod is not { } disposeSymbol
            || enumerator.FindImplementationForInterfaceMember(disposeSymbol) is not IMethodSymbol dispose
            || !SymbolEqualityComparer.Default.Equals(info.ElementType, item.Type)
            || !SymbolEqualityComparer.Default.Equals(current.Type, item.Type)
            || !ExactMethod(getEnumerator, collection, returnsVoid: false)
            || !ExactMethod(moveNext, enumerator, returnsVoid: false)
            || moveNext.ReturnType.SpecialType != SpecialType.System_Boolean
            || !ExactMethod(getter, enumerator, returnsVoid: false)
            || !ExactMethod(dispose, enumerator, returnsVoid: true))
        {
            return false;
        }

        plan = new(collection, enumerator, getEnumerator, moveNext, current, dispose, item);
        return true;
    }

    private static bool ExactMethod(IMethodSymbol method, INamedTypeSymbol owner, bool returnsVoid)
    {
        return SymbolEqualityComparer.Default.Equals(method.ContainingType, owner)
            && method.Parameters.Length == 0
            && method.DeclaredAccessibility == Accessibility.Public
            && !method.IsStatic && !method.IsVirtual && !method.IsOverride
            && method.ReturnsVoid == returnsVoid;
    }
}
