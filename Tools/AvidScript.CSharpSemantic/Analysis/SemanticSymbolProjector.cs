using System;
using System.Collections.Generic;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace AvidScript.CSharpSemantic;

internal static class SemanticSymbolProjector
{
    public static IReadOnlyList<SemanticSymbol> Project(
        SemanticCompilationContext context,
        SemanticTypeRegistry typeRegistry)
    {
        SemanticModel semanticModel = context.Compilation.GetSemanticModel(context.SyntaxTree, ignoreAccessibility: false);
        SyntaxNode root = context.SyntaxTree.GetRoot();
        Dictionary<string, SemanticSymbol> symbols = new(StringComparer.Ordinal);

        foreach (MemberDeclarationSyntax declaration in root.DescendantNodes().OfType<MemberDeclarationSyntax>())
        {
            ISymbol? symbol = semanticModel.GetDeclaredSymbol(declaration);
            if (symbol is not null)
            {
                AddSymbol(symbols, symbol, declaration, context, typeRegistry);
            }

            if (declaration is FieldDeclarationSyntax fieldDeclaration)
            {
                foreach (VariableDeclaratorSyntax variable in fieldDeclaration.Declaration.Variables)
                {
                    IFieldSymbol? field = semanticModel.GetDeclaredSymbol(variable) as IFieldSymbol;
                    if (field is not null)
                    {
                        AddSymbol(symbols, field, variable, context, typeRegistry);
                    }
                }
            }
        }

        foreach (ParameterSyntax parameterSyntax in root.DescendantNodes().OfType<ParameterSyntax>())
        {
            IParameterSymbol? parameter = semanticModel.GetDeclaredSymbol(parameterSyntax) as IParameterSymbol;
            if (parameter is not null)
            {
                AddSymbol(symbols, parameter, parameterSyntax, context, typeRegistry);
            }
        }

        return symbols.Values.OrderBy(symbol => symbol.Id, StringComparer.Ordinal).ToArray();
    }

    private static void AddSymbol(
        IDictionary<string, SemanticSymbol> symbols,
        ISymbol symbol,
        SyntaxNode syntax,
        SemanticCompilationContext context,
        SemanticTypeRegistry typeRegistry)
    {
        string id = GetSymbolId(symbol);
        string? typeId = GetType(symbol) is { } type ? typeRegistry.Register(type) : null;
        if (symbol is INamedTypeSymbol namedType)
        {
            typeId = typeRegistry.Register(namedType);
        }

        SemanticSymbol projected = new(
            id,
            GetKind(symbol),
            symbol.Name,
            GetContainingSymbolId(symbol),
            typeId,
            GetSignature(symbol),
            symbol.IsStatic,
            symbol.DeclaredAccessibility.ToString().ToLowerInvariant(),
            SemanticSpanFactory.Create(context.SourceText, syntax.Span));
        if (!symbols.TryAdd(id, projected))
        {
            throw new InvalidOperationException($"Duplicate stable semantic symbol id: {id}");
        }
    }

    public static string GetSymbolId(ISymbol symbol)
    {
        return symbol switch
        {
            INamedTypeSymbol type => "symbol:type:" + GetTypeIdentity(type),
            IFieldSymbol field => $"symbol:field:{GetTypeIdentity(field.ContainingType)}.{field.Name}:{SemanticTypeRegistry.GetCanonicalName(field.Type)}",
            IPropertySymbol property => $"symbol:property:{GetTypeIdentity(property.ContainingType)}.{property.Name}:{SemanticTypeRegistry.GetCanonicalName(property.Type)}",
            IMethodSymbol method => $"symbol:method:{GetTypeIdentity(method.ContainingType)}.{GetMethodSignature(method)}",
            IParameterSymbol parameter => $"symbol:parameter:{GetSymbolId(parameter.ContainingSymbol)}:{parameter.Ordinal}:{parameter.Name}:{SemanticTypeRegistry.GetCanonicalName(parameter.Type)}",
            _ => $"symbol:{GetKind(symbol)}:{symbol.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat)}",
        };
    }

    private static string? GetContainingSymbolId(ISymbol symbol)
    {
        if (symbol.ContainingSymbol is null || symbol.ContainingSymbol is INamespaceSymbol)
        {
            return null;
        }

        return GetSymbolId(symbol.ContainingSymbol);
    }

    private static ITypeSymbol? GetType(ISymbol symbol)
    {
        return symbol switch
        {
            IFieldSymbol field => field.Type,
            IPropertySymbol property => property.Type,
            IMethodSymbol method => method.ReturnType,
            IParameterSymbol parameter => parameter.Type,
            _ => null,
        };
    }

    private static string GetSignature(ISymbol symbol)
    {
        return symbol switch
        {
            INamedTypeSymbol type => GetTypeIdentity(type),
            IFieldSymbol field => $"{field.Name}:{SemanticTypeRegistry.GetCanonicalName(field.Type)}",
            IPropertySymbol property => $"{property.Name}:{SemanticTypeRegistry.GetCanonicalName(property.Type)}",
            IMethodSymbol method => GetMethodSignature(method),
            IParameterSymbol parameter => $"{parameter.Name}:{SemanticTypeRegistry.GetCanonicalName(parameter.Type)}",
            _ => symbol.ToDisplayString(SymbolDisplayFormat.MinimallyQualifiedFormat),
        };
    }

    private static string GetMethodSignature(IMethodSymbol method)
    {
        string parameters = string.Join(",", method.Parameters.Select(parameter => SemanticTypeRegistry.GetCanonicalName(parameter.Type)));
        return $"{method.Name}({parameters}):{SemanticTypeRegistry.GetCanonicalName(method.ReturnType)}";
    }

    private static string GetTypeIdentity(INamedTypeSymbol type)
    {
        return type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat);
    }

    private static string GetKind(ISymbol symbol)
    {
        return symbol switch
        {
            INamedTypeSymbol => "type",
            IFieldSymbol => "field",
            IPropertySymbol => "property",
            IMethodSymbol method when method.MethodKind == MethodKind.Constructor => "constructor",
            IMethodSymbol => "method",
            IParameterSymbol => "parameter",
            _ => symbol.Kind.ToString().ToLowerInvariant(),
        };
    }
}
