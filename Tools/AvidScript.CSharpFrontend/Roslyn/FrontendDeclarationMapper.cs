using System;
using System.Collections.Generic;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Text;

namespace AvidScript.CSharpFrontend;

internal static class FrontendDeclarationMapper
{
    public static IReadOnlyList<FrontendDeclaration> MapMembers(
        SyntaxList<MemberDeclarationSyntax> members,
        SourceText sourceText)
    {
        List<FrontendDeclaration> result = new();
        foreach (MemberDeclarationSyntax member in members)
        {
            switch (member)
            {
                case BaseNamespaceDeclarationSyntax declaration:
                    result.Add(MapNamespace(declaration, sourceText));
                    break;
                case TypeDeclarationSyntax declaration:
                    result.Add(MapType(declaration, sourceText));
                    break;
                case EnumDeclarationSyntax declaration:
                    result.Add(MapEnum(declaration, sourceText));
                    break;
                case FieldDeclarationSyntax declaration:
                    result.AddRange(MapFields(declaration, sourceText));
                    break;
                case EventFieldDeclarationSyntax declaration:
                    result.AddRange(MapEventFields(declaration, sourceText));
                    break;
                case MethodDeclarationSyntax declaration:
                    result.Add(MapMethod(declaration, sourceText));
                    break;
                case ConstructorDeclarationSyntax declaration:
                    result.Add(MapConstructor(declaration, sourceText));
                    break;
                case PropertyDeclarationSyntax declaration:
                    result.Add(MapProperty(declaration, sourceText));
                    break;
                case EventDeclarationSyntax declaration:
                    result.Add(MapEvent(declaration, sourceText));
                    break;
                case IndexerDeclarationSyntax declaration:
                    result.Add(MapIndexer(declaration, sourceText));
                    break;
                case OperatorDeclarationSyntax declaration:
                    result.Add(MapOperator(declaration, sourceText));
                    break;
                case ConversionOperatorDeclarationSyntax declaration:
                    result.Add(MapConversionOperator(declaration, sourceText));
                    break;
                case DelegateDeclarationSyntax declaration:
                    result.Add(MapDelegate(declaration, sourceText));
                    break;
                case GlobalStatementSyntax declaration:
                    result.Add(Create(
                        declaration,
                        sourceText,
                        string.Empty,
                        null,
                        body: FrontendAstNodeMapper.Map(declaration.Statement, sourceText)));
                    break;
                default:
                    result.Add(Create(
                        member,
                        sourceText,
                        string.Empty,
                        null,
                        body: FrontendAstNodeMapper.Map(member, sourceText)));
                    break;
            }
        }

        return result;
    }

    private static FrontendDeclaration MapNamespace(BaseNamespaceDeclarationSyntax declaration, SourceText sourceText)
    {
        return Create(
            declaration,
            sourceText,
            declaration.Name.ToString(),
            null,
            preamble: MapNamespacePreamble(declaration, sourceText),
            members: MapMembers(declaration.Members, sourceText));
    }

    private static FrontendDeclaration MapType(TypeDeclarationSyntax declaration, SourceText sourceText)
    {
        return Create(
            declaration,
            sourceText,
            declaration.Identifier.ValueText,
            null,
            typeParameters: MapTypeParameters(declaration.TypeParameterList),
            baseTypes: declaration.BaseList?.Types.Select(type => type.Type.ToString()).ToArray(),
            constraints: MapConstraints(declaration.ConstraintClauses),
            modifiers: MapModifiers(declaration.Modifiers),
            attributes: MapAttributes(declaration.AttributeLists, sourceText),
            members: MapMembers(declaration.Members, sourceText));
    }

    private static FrontendDeclaration MapEnum(EnumDeclarationSyntax declaration, SourceText sourceText)
    {
        FrontendDeclaration[] members = declaration.Members
            .Select(member => Create(
                member,
                sourceText,
                member.Identifier.ValueText,
                null,
                attributes: MapAttributes(member.AttributeLists, sourceText),
                initializer: FrontendAstNodeMapper.Map(member.EqualsValue?.Value, sourceText)))
            .ToArray();
        return Create(
            declaration,
            sourceText,
            declaration.Identifier.ValueText,
            null,
            modifiers: MapModifiers(declaration.Modifiers),
            attributes: MapAttributes(declaration.AttributeLists, sourceText),
            members: members);
    }

    private static IEnumerable<FrontendDeclaration> MapFields(FieldDeclarationSyntax declaration, SourceText sourceText)
    {
        foreach (VariableDeclaratorSyntax variable in declaration.Declaration.Variables)
        {
            yield return Create(
                declaration,
                sourceText,
                variable.Identifier.ValueText,
                declaration.Declaration.Type.ToString(),
                modifiers: MapModifiers(declaration.Modifiers),
                attributes: MapAttributes(declaration.AttributeLists, sourceText),
                initializer: FrontendAstNodeMapper.Map(variable.Initializer?.Value, sourceText));
        }
    }

    private static IEnumerable<FrontendDeclaration> MapEventFields(EventFieldDeclarationSyntax declaration, SourceText sourceText)
    {
        foreach (VariableDeclaratorSyntax variable in declaration.Declaration.Variables)
        {
            yield return Create(
                declaration,
                sourceText,
                variable.Identifier.ValueText,
                declaration.Declaration.Type.ToString(),
                modifiers: MapModifiers(declaration.Modifiers),
                attributes: MapAttributes(declaration.AttributeLists, sourceText),
                initializer: FrontendAstNodeMapper.Map(variable.Initializer?.Value, sourceText));
        }
    }

    private static FrontendDeclaration MapMethod(MethodDeclarationSyntax declaration, SourceText sourceText)
    {
        return Create(
            declaration,
            sourceText,
            declaration.Identifier.ValueText,
            declaration.ReturnType.ToString(),
            typeParameters: MapTypeParameters(declaration.TypeParameterList),
            constraints: MapConstraints(declaration.ConstraintClauses),
            explicitInterface: declaration.ExplicitInterfaceSpecifier?.Name.ToString(),
            modifiers: MapModifiers(declaration.Modifiers),
            attributes: MapAttributes(declaration.AttributeLists, sourceText),
            parameters: MapParameters(declaration.ParameterList.Parameters, sourceText),
            body: FrontendAstNodeMapper.Map(declaration.Body ?? (SyntaxNode?)declaration.ExpressionBody, sourceText));
    }

    private static FrontendDeclaration MapConstructor(ConstructorDeclarationSyntax declaration, SourceText sourceText)
    {
        return Create(
            declaration,
            sourceText,
            declaration.Identifier.ValueText,
            null,
            modifiers: MapModifiers(declaration.Modifiers),
            attributes: MapAttributes(declaration.AttributeLists, sourceText),
            parameters: MapParameters(declaration.ParameterList.Parameters, sourceText),
            body: FrontendAstNodeMapper.Map(declaration.Body ?? (SyntaxNode?)declaration.ExpressionBody, sourceText));
    }

    private static FrontendDeclaration MapProperty(PropertyDeclarationSyntax declaration, SourceText sourceText)
    {
        return Create(
            declaration,
            sourceText,
            declaration.Identifier.ValueText,
            declaration.Type.ToString(),
            explicitInterface: declaration.ExplicitInterfaceSpecifier?.Name.ToString(),
            modifiers: MapModifiers(declaration.Modifiers),
            attributes: MapAttributes(declaration.AttributeLists, sourceText),
            initializer: FrontendAstNodeMapper.Map(declaration.Initializer?.Value, sourceText),
            body: FrontendAstNodeMapper.Map(declaration.AccessorList ?? (SyntaxNode?)declaration.ExpressionBody, sourceText));
    }

    private static FrontendDeclaration MapEvent(EventDeclarationSyntax declaration, SourceText sourceText)
    {
        return Create(
            declaration,
            sourceText,
            declaration.Identifier.ValueText,
            declaration.Type.ToString(),
            explicitInterface: declaration.ExplicitInterfaceSpecifier?.Name.ToString(),
            modifiers: MapModifiers(declaration.Modifiers),
            attributes: MapAttributes(declaration.AttributeLists, sourceText),
            body: FrontendAstNodeMapper.Map(declaration.AccessorList, sourceText));
    }

    private static FrontendDeclaration MapIndexer(IndexerDeclarationSyntax declaration, SourceText sourceText)
    {
        return Create(
            declaration,
            sourceText,
            "this",
            declaration.Type.ToString(),
            explicitInterface: declaration.ExplicitInterfaceSpecifier?.Name.ToString(),
            modifiers: MapModifiers(declaration.Modifiers),
            attributes: MapAttributes(declaration.AttributeLists, sourceText),
            parameters: MapParameters(declaration.ParameterList.Parameters, sourceText),
            body: FrontendAstNodeMapper.Map(declaration.AccessorList ?? (SyntaxNode?)declaration.ExpressionBody, sourceText));
    }

    private static FrontendDeclaration MapOperator(OperatorDeclarationSyntax declaration, SourceText sourceText)
    {
        return Create(
            declaration,
            sourceText,
            $"operator {declaration.OperatorToken.Text}",
            declaration.ReturnType.ToString(),
            modifiers: MapModifiers(declaration.Modifiers),
            attributes: MapAttributes(declaration.AttributeLists, sourceText),
            parameters: MapParameters(declaration.ParameterList.Parameters, sourceText),
            body: FrontendAstNodeMapper.Map(declaration.Body ?? (SyntaxNode?)declaration.ExpressionBody, sourceText));
    }

    private static FrontendDeclaration MapConversionOperator(ConversionOperatorDeclarationSyntax declaration, SourceText sourceText)
    {
        return Create(
            declaration,
            sourceText,
            $"{declaration.ImplicitOrExplicitKeyword.ValueText} operator",
            declaration.Type.ToString(),
            modifiers: MapModifiers(declaration.Modifiers),
            attributes: MapAttributes(declaration.AttributeLists, sourceText),
            parameters: MapParameters(declaration.ParameterList.Parameters, sourceText),
            body: FrontendAstNodeMapper.Map(declaration.Body ?? (SyntaxNode?)declaration.ExpressionBody, sourceText));
    }

    private static FrontendDeclaration MapDelegate(DelegateDeclarationSyntax declaration, SourceText sourceText)
    {
        return Create(
            declaration,
            sourceText,
            declaration.Identifier.ValueText,
            declaration.ReturnType.ToString(),
            typeParameters: MapTypeParameters(declaration.TypeParameterList),
            constraints: MapConstraints(declaration.ConstraintClauses),
            modifiers: MapModifiers(declaration.Modifiers),
            attributes: MapAttributes(declaration.AttributeLists, sourceText),
            parameters: MapParameters(declaration.ParameterList.Parameters, sourceText));
    }

    private static IReadOnlyList<FrontendParameter> MapParameters(
        SeparatedSyntaxList<ParameterSyntax> parameters,
        SourceText sourceText)
    {
        return parameters
            .Select(parameter => new FrontendParameter(
                parameter.Identifier.ValueText,
                parameter.Type?.ToString() ?? string.Empty,
                MapModifiers(parameter.Modifiers),
                FrontendSpanFactory.Create(sourceText, parameter.Span),
                FrontendAstNodeMapper.Map(parameter.Default?.Value, sourceText)))
            .ToArray();
    }

    private static IReadOnlyList<FrontendAttribute> MapAttributes(
        SyntaxList<AttributeListSyntax> lists,
        SourceText sourceText)
    {
        return lists
            .SelectMany(list => list.Attributes)
            .Select(attribute => new FrontendAttribute(
                attribute.Name.ToString(),
                FrontendSpanFactory.Create(sourceText, attribute.Span),
                attribute.ArgumentList?.Arguments
                    .Select(argument => FrontendAstNodeMapper.Map(argument.Expression, sourceText))
                    .Where(argument => argument is not null)
                    .Cast<FrontendAstNode>()
                    .ToArray()
                ?? Array.Empty<FrontendAstNode>()))
            .ToArray();
    }

    private static IReadOnlyList<FrontendAstNode> MapNamespacePreamble(BaseNamespaceDeclarationSyntax declaration, SourceText sourceText)
    {
        return FrontendAstNodeMapper.MapMany(
            declaration.Externs.Cast<SyntaxNode?>().Concat(declaration.Usings),
            sourceText);
    }

    private static IReadOnlyList<string> MapTypeParameters(TypeParameterListSyntax? list)
    {
        return list?.Parameters.Select(parameter => parameter.Identifier.ValueText).ToArray()
            ?? Array.Empty<string>();
    }

    private static IReadOnlyList<string> MapConstraints(SyntaxList<TypeParameterConstraintClauseSyntax> clauses)
    {
        return clauses.Select(clause => clause.ToString()).ToArray();
    }

    private static IReadOnlyList<string> MapModifiers(SyntaxTokenList modifiers)
    {
        return modifiers.Select(token => token.Text).ToArray();
    }

    private static FrontendDeclaration Create(
        SyntaxNode node,
        SourceText sourceText,
        string name,
        string? type,
        IReadOnlyList<string>? typeParameters = null,
        IReadOnlyList<string>? baseTypes = null,
        IReadOnlyList<string>? constraints = null,
        string? explicitInterface = null,
        IReadOnlyList<FrontendAstNode>? preamble = null,
        IReadOnlyList<string>? modifiers = null,
        IReadOnlyList<FrontendAttribute>? attributes = null,
        IReadOnlyList<FrontendParameter>? parameters = null,
        IReadOnlyList<FrontendDeclaration>? members = null,
        FrontendAstNode? initializer = null,
        FrontendAstNode? body = null)
    {
        return new FrontendDeclaration(
            node.Kind().ToString(),
            name,
            type,
            typeParameters ?? Array.Empty<string>(),
            baseTypes ?? Array.Empty<string>(),
            constraints ?? Array.Empty<string>(),
            explicitInterface,
            preamble ?? Array.Empty<FrontendAstNode>(),
            modifiers ?? Array.Empty<string>(),
            FrontendSpanFactory.Create(sourceText, node.Span),
            attributes ?? Array.Empty<FrontendAttribute>(),
            parameters ?? Array.Empty<FrontendParameter>(),
            members ?? Array.Empty<FrontendDeclaration>(),
            initializer,
            body);
    }
}
