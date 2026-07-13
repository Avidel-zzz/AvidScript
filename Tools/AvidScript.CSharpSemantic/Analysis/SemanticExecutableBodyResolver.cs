using System.Collections.Generic;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace AvidScript.CSharpSemantic;

internal sealed record SemanticExecutableBody(
    SyntaxNode Declaration,
    IMethodSymbol Method,
    IOperation Operation);

internal static class SemanticExecutableBodyResolver
{
    public static IReadOnlyList<SemanticExecutableBody> Resolve(SemanticCompilationContext context)
    {
        SemanticModel semanticModel = context.Compilation.GetSemanticModel(context.SyntaxTree, ignoreAccessibility: false);
        SyntaxNode root = context.SyntaxTree.GetRoot();
        List<SemanticExecutableBody> bodies = new();

        foreach (SyntaxNode declaration in root.DescendantNodes().Where(IsExecutableDeclaration))
        {
            IMethodSymbol? method = GetMethodSymbol(declaration, semanticModel);
            IOperation? operation = GetOperation(declaration, semanticModel);
            if (method is null || operation is null)
            {
                continue;
            }

            while (operation.Parent is { } parent)
            {
                operation = parent;
            }

            bodies.Add(new SemanticExecutableBody(declaration, method, operation));
        }

        return bodies;
    }

    private static bool IsExecutableDeclaration(SyntaxNode node)
    {
        return node is BaseMethodDeclarationSyntax or AccessorDeclarationSyntax ||
            node is PropertyDeclarationSyntax { ExpressionBody: not null } ||
            node is IndexerDeclarationSyntax { ExpressionBody: not null };
    }

    private static IMethodSymbol? GetMethodSymbol(SyntaxNode declaration, SemanticModel semanticModel)
    {
        return declaration switch
        {
            PropertyDeclarationSyntax property =>
                (semanticModel.GetDeclaredSymbol(property) as IPropertySymbol)?.GetMethod,
            IndexerDeclarationSyntax indexer =>
                (semanticModel.GetDeclaredSymbol(indexer) as IPropertySymbol)?.GetMethod,
            _ => semanticModel.GetDeclaredSymbol(declaration) as IMethodSymbol,
        };
    }

    private static IOperation? GetOperation(SyntaxNode declaration, SemanticModel semanticModel)
    {
        return declaration switch
        {
            PropertyDeclarationSyntax property => semanticModel.GetOperation(property.ExpressionBody!.Expression),
            IndexerDeclarationSyntax indexer => semanticModel.GetOperation(indexer.ExpressionBody!.Expression),
            _ => semanticModel.GetOperation(declaration),
        };
    }
}
