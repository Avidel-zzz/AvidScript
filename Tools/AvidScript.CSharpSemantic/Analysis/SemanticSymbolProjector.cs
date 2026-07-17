using System;
using System.Collections.Generic;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Text;

namespace AvidScript.CSharpSemantic;

internal static class SemanticSymbolProjector
{
    public static IReadOnlyList<SemanticSymbol> Project(
        SemanticCompilationContext context,
        SemanticTypeRegistry typeRegistry)
    {
        Dictionary<string, SemanticSymbol> symbols = new(StringComparer.Ordinal);
        foreach (SemanticCompilationUnit unit in context.ProjectionUnits)
        {
            SemanticModel semanticModel = context.Compilation.GetSemanticModel(
                unit.SyntaxTree,
                ignoreAccessibility: false);
            ProjectUnit(unit.SyntaxTree.GetRoot(), semanticModel, unit.SourceText, symbols, typeRegistry);
        }

        return symbols.Values.OrderBy(symbol => symbol.Id, StringComparer.Ordinal).ToArray();
    }

    private static void ProjectUnit(
        SyntaxNode root,
        SemanticModel semanticModel,
        SourceText sourceText,
        IDictionary<string, SemanticSymbol> symbols,
        SemanticTypeRegistry typeRegistry)
    {
        foreach (MemberDeclarationSyntax declaration in root.DescendantNodes().OfType<MemberDeclarationSyntax>())
        {
            ISymbol? symbol = semanticModel.GetDeclaredSymbol(declaration);
            if (symbol is not null)
            {
                AddSymbol(symbols, symbol, declaration, sourceText, typeRegistry);
            }

            if (declaration is FieldDeclarationSyntax fieldDeclaration)
            {
                foreach (VariableDeclaratorSyntax variable in fieldDeclaration.Declaration.Variables)
                {
                    IFieldSymbol? field = semanticModel.GetDeclaredSymbol(variable) as IFieldSymbol;
                    if (field is not null)
                    {
                        AddSymbol(symbols, field, variable, sourceText, typeRegistry);
                    }
                }
            }
        }

        foreach (ParameterSyntax parameterSyntax in root.DescendantNodes().OfType<ParameterSyntax>())
        {
            IParameterSymbol? parameter = semanticModel.GetDeclaredSymbol(parameterSyntax) as IParameterSymbol;
            if (parameter is not null)
            {
                AddSymbol(symbols, parameter, parameterSyntax, sourceText, typeRegistry);
            }
        }

        foreach (AccessorDeclarationSyntax accessorSyntax in root.DescendantNodes().OfType<AccessorDeclarationSyntax>())
        {
            IMethodSymbol? accessor = semanticModel.GetDeclaredSymbol(accessorSyntax) as IMethodSymbol;
            if (accessor is not null)
            {
                AddSymbol(symbols, accessor, accessorSyntax, sourceText, typeRegistry);
            }
        }
        foreach (PropertyDeclarationSyntax propertySyntax in root.DescendantNodes()
            .OfType<PropertyDeclarationSyntax>()
            .Where(property => property.ExpressionBody is not null))
        {
            IPropertySymbol? property = semanticModel.GetDeclaredSymbol(propertySyntax) as IPropertySymbol;
            if (property?.GetMethod is { } getter)
            {
                AddSymbol(symbols, getter, propertySyntax.ExpressionBody!, sourceText, typeRegistry);
            }
        }
        foreach (IndexerDeclarationSyntax indexerSyntax in root.DescendantNodes()
            .OfType<IndexerDeclarationSyntax>()
            .Where(indexer => indexer.ExpressionBody is not null))
        {
            IPropertySymbol? indexer = semanticModel.GetDeclaredSymbol(indexerSyntax) as IPropertySymbol;
            if (indexer?.GetMethod is { } getter)
            {
                AddSymbol(symbols, getter, indexerSyntax.ExpressionBody!, sourceText, typeRegistry);
            }
        }
        foreach (VariableDeclaratorSyntax variable in root.DescendantNodes().OfType<VariableDeclaratorSyntax>())
        {
            ILocalSymbol? local = semanticModel.GetDeclaredSymbol(variable) as ILocalSymbol;
            if (local is not null)
            {
                AddSymbol(symbols, local, variable, sourceText, typeRegistry);
            }
        }

        foreach (SingleVariableDesignationSyntax designation in root.DescendantNodes().OfType<SingleVariableDesignationSyntax>())
        {
            ILocalSymbol? local = semanticModel.GetDeclaredSymbol(designation) as ILocalSymbol;
            if (local is not null)
            {
                AddSymbol(symbols, local, designation, sourceText, typeRegistry);
            }
        }

    }

    private static void AddSymbol(
        IDictionary<string, SemanticSymbol> symbols,
        ISymbol symbol,
        SyntaxNode syntax,
        SourceText sourceText,
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
            SemanticSpanFactory.Create(sourceText, syntax.Span));
        if (!symbols.TryAdd(id, projected))
        {
            if (symbol is INamespaceSymbol)
            {
                return;
            }

            throw new InvalidOperationException($"Duplicate stable semantic symbol id: {id}");
        }
    }

    public static string GetSymbolId(ISymbol symbol)
    {
        return symbol switch
        {
            INamedTypeSymbol type => "symbol:type:" + GetTypeIdentity(type),
            IFieldSymbol field => GetFieldId(field),
            IPropertySymbol property => GetPropertyId(property),
            IMethodSymbol method => GetMethodId(method),
            IParameterSymbol parameter => GetParameterId(parameter),
            ILocalSymbol local => $"symbol:local:{GetSymbolId(local.ContainingSymbol)}.local:{GetSourceStart(local)}:{local.Name}:{SemanticTypeRegistry.GetCanonicalName(local.Type)}",
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
            ILocalSymbol local => local.Type,
            _ => null,
        };
    }

    private static string GetSignature(ISymbol symbol)
    {
        return symbol switch
        {
            INamedTypeSymbol type => GetTypeIdentity(type),
            IFieldSymbol field => $"{field.Name}:{SemanticTypeRegistry.GetCanonicalName(field.Type)}",
            IPropertySymbol property => GetPropertySignature(property.OriginalDefinition),
            IMethodSymbol method => GetMethodSignature(method),
            IParameterSymbol parameter => $"{parameter.Name}:{SemanticTypeRegistry.GetCanonicalName(parameter.Type)}",
            ILocalSymbol local => $"{local.Name}:{SemanticTypeRegistry.GetCanonicalName(local.Type)}",
            _ => symbol.ToDisplayString(SymbolDisplayFormat.MinimallyQualifiedFormat),
        };
    }

    private static string GetFieldId(IFieldSymbol field)
    {
        IFieldSymbol definition = field.OriginalDefinition;
        return $"symbol:field:{GetTypeIdentity(definition.ContainingType)}.{definition.Name}:{SemanticTypeRegistry.GetCanonicalName(definition.Type)}";
    }

    private static string GetMethodId(IMethodSymbol method)
    {
        IMethodSymbol definition = method.OriginalDefinition;
        return $"symbol:method:{GetTypeIdentity(definition.ContainingType)}.{GetMethodSignature(definition)}";
    }

    private static string GetParameterId(IParameterSymbol parameter)
    {
        IParameterSymbol definition = parameter.OriginalDefinition;
        return $"symbol:parameter:{GetSymbolId(definition.ContainingSymbol)}:{definition.Ordinal}:{definition.Name}:{SemanticTypeRegistry.GetCanonicalName(definition.Type)}";
    }
    private static string GetPropertyId(IPropertySymbol property)
    {
        IPropertySymbol definition = property.OriginalDefinition;
        return $"symbol:property:{GetTypeIdentity(definition.ContainingType)}.{GetPropertySignature(definition)}";
    }

    private static string GetPropertySignature(IPropertySymbol property)
    {
        string name = property.IsIndexer
            ? $"this[{string.Join(",", property.Parameters.Select(GetParameterSignature))}]"
            : property.Name;
        return $"{name}:{SemanticTypeRegistry.GetCanonicalName(property.Type)}";
    }
    private static string GetMethodSignature(IMethodSymbol method)
    {
        IMethodSymbol definition = method.OriginalDefinition;
        string name = definition.Arity == 0
            ? definition.Name
            : $"{definition.Name}`{definition.Arity}";
        string parameters = string.Join(",", definition.Parameters.Select(GetParameterSignature));
        return $"{name}({parameters}):{SemanticTypeRegistry.GetCanonicalName(definition.ReturnType)}";
    }

    private static string GetParameterSignature(IParameterSymbol parameter)
    {
        string modifier = parameter.RefKind switch
        {
            RefKind.Ref => "ref ",
            RefKind.Out => "out ",
            RefKind.In => "in ",
            _ => string.Empty,
        };
        return modifier + SemanticTypeRegistry.GetCanonicalName(parameter.Type);
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
            ILocalSymbol => "local",
            _ => symbol.Kind.ToString().ToLowerInvariant(),
        };
    }

    private static int GetSourceStart(ISymbol symbol)
    {
        return symbol.Locations.FirstOrDefault(location => location.IsInSource)?.SourceSpan.Start ?? -1;
    }
}
