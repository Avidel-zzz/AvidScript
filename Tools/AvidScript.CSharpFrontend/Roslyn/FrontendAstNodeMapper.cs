using System;
using System.Collections.Generic;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Text;

namespace AvidScript.CSharpFrontend;

internal static class FrontendAstNodeMapper
{
    public static FrontendAstNode? Map(SyntaxNode? syntax, SourceText sourceText)
    {
        if (syntax is null)
        {
            return null;
        }

        return syntax switch
        {
            BlockSyntax node => Create(node, sourceText, children: MapMany(node.Statements, sourceText)),
            ExpressionStatementSyntax node => Create(node, sourceText, children: MapMany(new[] { node.Expression }, sourceText)),
            LocalDeclarationStatementSyntax node => MapLocalDeclaration(node, sourceText),
            ReturnStatementSyntax node => Create(node, sourceText, children: MapMany(new[] { node.Expression }, sourceText)),
            IfStatementSyntax node => Create(node, sourceText, children: MapMany(new SyntaxNode?[] { node.Condition, node.Statement, node.Else }, sourceText)),
            ElseClauseSyntax node => Create(node, sourceText, children: MapMany(new[] { node.Statement }, sourceText)),
            WhileStatementSyntax node => Create(node, sourceText, children: MapMany(new SyntaxNode?[] { node.Condition, node.Statement }, sourceText)),
            DoStatementSyntax node => Create(node, sourceText, children: MapMany(new SyntaxNode?[] { node.Statement, node.Condition }, sourceText)),
            ForStatementSyntax node => MapForStatement(node, sourceText),
            ForEachStatementSyntax node => Create(node, sourceText, name: node.Identifier.ValueText, type: node.Type.ToString(), children: MapMany(new SyntaxNode?[] { node.Expression, node.Statement }, sourceText)),
            BreakStatementSyntax node => Create(node, sourceText),
            ContinueStatementSyntax node => Create(node, sourceText),
            ThrowStatementSyntax node => Create(node, sourceText, children: MapMany(new[] { node.Expression }, sourceText)),
            EmptyStatementSyntax node => Create(node, sourceText),
            UsingDirectiveSyntax node => Create(
                node,
                sourceText,
                name: node.Alias?.Name.Identifier.ValueText,
                type: node.Name?.ToString(),
                modifiers: Tokens(node.GlobalKeyword, node.StaticKeyword)),
            ExternAliasDirectiveSyntax node => Create(node, sourceText, name: node.Identifier.ValueText),
            AttributeListSyntax node => Create(node, sourceText, children: MapMany(node.Attributes, sourceText)),
            AttributeSyntax node => Create(node, sourceText, name: node.Name.ToString(), children: MapMany(node.ArgumentList?.Arguments, sourceText)),
            AttributeArgumentSyntax node => Create(node, sourceText, name: node.NameEquals?.Name.Identifier.ValueText ?? node.NameColon?.Name.Identifier.ValueText, children: MapMany(new[] { node.Expression }, sourceText)),
            VariableDeclaratorSyntax node => Create(node, sourceText, name: node.Identifier.ValueText, children: MapMany(new[] { node.Initializer }, sourceText)),
            EqualsValueClauseSyntax node => Create(node, sourceText, children: MapMany(new[] { node.Value }, sourceText)),
            AssignmentExpressionSyntax node => Create(node, sourceText, @operator: node.OperatorToken.Text, children: MapMany(new SyntaxNode?[] { node.Left, node.Right }, sourceText)),
            BinaryExpressionSyntax node => Create(node, sourceText, @operator: node.OperatorToken.Text, children: MapMany(new SyntaxNode?[] { node.Left, node.Right }, sourceText)),
            InvocationExpressionSyntax node => Create(node, sourceText, name: GetInvocationName(node.Expression), children: MapMany(new SyntaxNode?[] { node.Expression }.Concat(node.ArgumentList.Arguments), sourceText)),
            ArgumentSyntax node => Create(node, sourceText, name: node.NameColon?.Name.Identifier.ValueText, @operator: node.RefKindKeyword.Text, children: MapMany(new[] { node.Expression }, sourceText)),
            MemberAccessExpressionSyntax node => Create(node, sourceText, name: node.Name.ToString(), @operator: node.OperatorToken.Text, children: MapMany(new[] { node.Expression }, sourceText)),
            MemberBindingExpressionSyntax node => Create(node, sourceText, name: node.Name.ToString(), @operator: node.OperatorToken.Text),
            ObjectCreationExpressionSyntax node => MapObjectCreation(node, sourceText),
            ImplicitObjectCreationExpressionSyntax node => MapImplicitObjectCreation(node, sourceText),
            LiteralExpressionSyntax node => Create(node, sourceText, value: node.Token.ValueText, text: node.Token.Text),
            IdentifierNameSyntax node => Create(node, sourceText, name: node.Identifier.ValueText),
            GenericNameSyntax node => Create(node, sourceText, name: node.Identifier.ValueText, children: MapMany(node.TypeArgumentList.Arguments, sourceText)),
            ParenthesizedExpressionSyntax node => Create(node, sourceText, children: MapMany(new[] { node.Expression }, sourceText)),
            PrefixUnaryExpressionSyntax node => Create(node, sourceText, @operator: node.OperatorToken.Text, children: MapMany(new[] { node.Operand }, sourceText)),
            PostfixUnaryExpressionSyntax node => Create(node, sourceText, @operator: node.OperatorToken.Text, children: MapMany(new[] { node.Operand }, sourceText)),
            ConditionalExpressionSyntax node => Create(node, sourceText, children: MapMany(new SyntaxNode?[] { node.Condition, node.WhenTrue, node.WhenFalse }, sourceText)),
            CastExpressionSyntax node => Create(node, sourceText, type: node.Type.ToString(), children: MapMany(new[] { node.Expression }, sourceText)),
            ElementAccessExpressionSyntax node => Create(node, sourceText, children: MapMany(new SyntaxNode?[] { node.Expression }.Concat(node.ArgumentList.Arguments), sourceText)),
            ThisExpressionSyntax node => Create(node, sourceText, name: "this"),
            BaseExpressionSyntax node => Create(node, sourceText, name: "base"),
            DeclarationExpressionSyntax node => Create(node, sourceText, type: node.Type.ToString(), children: MapMany(new[] { node.Designation }, sourceText)),
            SingleVariableDesignationSyntax node => Create(node, sourceText, name: node.Identifier.ValueText),
            ParenthesizedVariableDesignationSyntax node => Create(node, sourceText, children: MapMany(node.Variables, sourceText)),
            RefExpressionSyntax node => Create(node, sourceText, @operator: node.RefKeyword.Text, children: MapMany(new[] { node.Expression }, sourceText)),
            AwaitExpressionSyntax node => Create(node, sourceText, @operator: node.AwaitKeyword.Text, children: MapMany(new[] { node.Expression }, sourceText)),
            ConditionalAccessExpressionSyntax node => Create(node, sourceText, children: MapMany(new SyntaxNode?[] { node.Expression, node.WhenNotNull }, sourceText)),
            DefaultExpressionSyntax node => Create(node, sourceText, type: node.Type.ToString()),
            InitializerExpressionSyntax node => Create(node, sourceText, children: MapMany(node.Expressions, sourceText)),
            AccessorListSyntax node => Create(node, sourceText, children: MapMany(node.Accessors, sourceText)),
            AccessorDeclarationSyntax node => Create(node, sourceText, name: node.Keyword.ValueText, children: MapMany(new SyntaxNode?[] { node.Body, node.ExpressionBody }, sourceText)),
            ArrowExpressionClauseSyntax node => Create(node, sourceText, children: MapMany(new[] { node.Expression }, sourceText)),
            _ => Create(syntax, sourceText, isSupported: false, text: syntax.ToString()),
        };
    }

    private static FrontendAstNode MapLocalDeclaration(LocalDeclarationStatementSyntax node, SourceText sourceText)
    {
        FrontendAstNode[] variables = MapMany(node.Declaration.Variables, sourceText);
        string? name = node.Declaration.Variables.Count == 1
            ? node.Declaration.Variables[0].Identifier.ValueText
            : null;
        IReadOnlyList<string> modifiers = node.Modifiers
            .Select(token => token.Text)
            .Concat(Tokens(node.AwaitKeyword, node.UsingKeyword))
            .Where(text => !string.IsNullOrEmpty(text))
            .ToArray();
        return Create(node, sourceText, name: name, type: node.Declaration.Type.ToString(), modifiers: modifiers, children: variables);
    }

    private static FrontendAstNode MapObjectCreation(ObjectCreationExpressionSyntax node, SourceText sourceText)
    {
        IEnumerable<SyntaxNode?> children = node.ArgumentList?.Arguments.Cast<SyntaxNode?>()
            ?? Enumerable.Empty<SyntaxNode?>();
        return Create(node, sourceText, type: node.Type.ToString(), children: MapMany(children.Append(node.Initializer), sourceText));
    }

    private static FrontendAstNode MapImplicitObjectCreation(ImplicitObjectCreationExpressionSyntax node, SourceText sourceText)
    {
        return Create(node, sourceText, children: MapMany(node.ArgumentList.Arguments.Cast<SyntaxNode?>().Append(node.Initializer), sourceText));
    }

    private static FrontendAstNode MapForStatement(ForStatementSyntax node, SourceText sourceText)
    {
        IEnumerable<SyntaxNode?> children = node.Declaration?.Variables.Cast<SyntaxNode?>()
            ?? Enumerable.Empty<SyntaxNode?>();
        children = children
            .Concat(node.Initializers)
            .Append(node.Condition)
            .Concat(node.Incrementors)
            .Append(node.Statement);
        return Create(node, sourceText, type: node.Declaration?.Type.ToString(), children: MapMany(children, sourceText));
    }

    private static string? GetInvocationName(ExpressionSyntax expression)
    {
        return expression switch
        {
            IdentifierNameSyntax identifier => identifier.Identifier.ValueText,
            GenericNameSyntax generic => generic.Identifier.ValueText,
            MemberAccessExpressionSyntax member => member.Name.ToString(),
            MemberBindingExpressionSyntax binding => binding.Name.ToString(),
            _ => null,
        };
    }

    public static FrontendAstNode[] MapMany(IEnumerable<SyntaxNode?>? nodes, SourceText sourceText)
    {
        if (nodes is null)
        {
            return Array.Empty<FrontendAstNode>();
        }

        return nodes
            .Select(node => Map(node, sourceText))
            .Where(node => node is not null)
            .Cast<FrontendAstNode>()
            .ToArray();
    }

    private static FrontendAstNode Create(
        SyntaxNode node,
        SourceText sourceText,
        bool isSupported = true,
        string? name = null,
        string? type = null,
        IReadOnlyList<string>? modifiers = null,
        string? @operator = null,
        string? value = null,
        string? text = null,
        IReadOnlyList<FrontendAstNode>? children = null)
    {
        return new FrontendAstNode(
            node.Kind().ToString(),
            isSupported,
            name,
            type,
            modifiers ?? Array.Empty<string>(),
            @operator,
            value,
            text,
            FrontendSpanFactory.Create(sourceText, node.Span),
            children ?? Array.Empty<FrontendAstNode>());
    }

    private static IReadOnlyList<string> Tokens(params SyntaxToken[] tokens)
    {
        return tokens
            .Where(token => !token.IsKind(Microsoft.CodeAnalysis.CSharp.SyntaxKind.None))
            .Select(token => token.Text)
            .Where(text => !string.IsNullOrEmpty(text))
            .ToArray();
    }
}
