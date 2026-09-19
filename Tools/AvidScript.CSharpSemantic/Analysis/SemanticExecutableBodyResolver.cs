using System.Collections.Generic;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Operations;

namespace AvidScript.CSharpSemantic;

internal sealed record SemanticExecutableBody(
    SyntaxNode Declaration,
    IMethodSymbol Method,
    IOperation Operation,
    SemanticCompilationUnit Unit);

internal static class SemanticExecutableBodyResolver
{
    public static IReadOnlyList<SemanticExecutableBody> Resolve(SemanticCompilationContext context)
    {
        List<SemanticExecutableBody> bodies = new();
        foreach (SemanticCompilationUnit unit in context.ProjectionUnits)
        {
            SemanticModel semanticModel = context.Compilation.GetSemanticModel(
                unit.SyntaxTree,
                ignoreAccessibility: false);
            SyntaxNode root = unit.SyntaxTree.GetRoot();

            foreach (SyntaxNode declaration in root.DescendantNodes().Where(IsExecutableDeclaration))
            {
                IMethodSymbol? method = GetMethodSymbol(declaration, semanticModel);
                IOperation? operation = GetOperation(declaration, semanticModel);
                if (method is null || operation is null)
                {
                    continue;
                }

                if (method.MethodKind == MethodKind.LocalFunction
                    && (!SemanticLocalFunctionPolicy.IsSupported(method)
                        || operation is not ILocalFunctionOperation { Body: not null }))
                {
                    continue;
                }
                if (method.MethodKind == MethodKind.AnonymousFunction && !SemanticLambdaPolicy.IsSupported(method)) continue;

                while (!IsLexicalMethod(method) && operation.Parent is { } parent)
                {
                    operation = parent;
                }

                bodies.Add(new SemanticExecutableBody(declaration, method, operation, unit));
            }
        }

        return bodies;
    }

    internal static bool IsLexicalMethod(IMethodSymbol method) =>
        method.MethodKind is MethodKind.LocalFunction or MethodKind.AnonymousFunction;

    internal static bool IsExecutableDeclaration(SyntaxNode node)
    {
        return node is BaseMethodDeclarationSyntax or AccessorDeclarationSyntax or LocalFunctionStatementSyntax or AnonymousFunctionExpressionSyntax ||
            node is PropertyDeclarationSyntax { ExpressionBody: not null } ||
            node is IndexerDeclarationSyntax { ExpressionBody: not null };
    }

    internal static IMethodSymbol? GetMethodSymbol(SyntaxNode declaration, SemanticModel semanticModel)
    {
        return declaration switch
        {
            AnonymousFunctionExpressionSyntax lambda => (semanticModel.GetOperation(lambda) as IAnonymousFunctionOperation)?.Symbol,
            PropertyDeclarationSyntax property =>
                (semanticModel.GetDeclaredSymbol(property) as IPropertySymbol)?.GetMethod,
            IndexerDeclarationSyntax indexer =>
                (semanticModel.GetDeclaredSymbol(indexer) as IPropertySymbol)?.GetMethod,
            _ => semanticModel.GetDeclaredSymbol(declaration) as IMethodSymbol,
        };
    }

    internal static IOperation? GetOperation(SyntaxNode declaration, SemanticModel semanticModel)
    {
        return declaration switch
        {
            PropertyDeclarationSyntax property => semanticModel.GetOperation(property.ExpressionBody!.Expression),
            IndexerDeclarationSyntax indexer => semanticModel.GetOperation(indexer.ExpressionBody!.Expression),
            _ => semanticModel.GetOperation(declaration),
        };
    }
}
