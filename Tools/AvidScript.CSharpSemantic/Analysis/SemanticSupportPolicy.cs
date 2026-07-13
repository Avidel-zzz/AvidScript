using System;
using System.Collections.Generic;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Operations;

namespace AvidScript.CSharpSemantic;

internal sealed record SemanticSupportDecision(
    bool IsSupported,
    string? DiagnosticCode,
    string? DiagnosticMessage);

internal sealed record SemanticSupportProjection(
    IReadOnlyList<SemanticDiagnostic> Diagnostics);

internal static class SemanticSupportPolicy
{
    public static SemanticSupportDecision EvaluateOperation(
        IOperation operation,
        string stableKind,
        bool hasStableProjection)
    {
        if (operation is IAnonymousFunctionOperation or ILocalFunctionOperation or IDelegateCreationOperation)
        {
            return Unsupported(
                "ASCS4001",
                "Lambda, local-function, delegate, and closure semantics are not supported by the current AvidScript semantic profile.");
        }

        if (operation is IDynamicInvocationOperation or IDynamicMemberReferenceOperation or
            IDynamicIndexerAccessOperation or IDynamicObjectCreationOperation ||
            operation.Type is IDynamicTypeSymbol)
        {
            return Unsupported(
                "ASCS4002",
                "Dynamic binding is not supported by the current AvidScript semantic profile.");
        }

        if (operation is IAddressOfOperation or IFunctionPointerInvocationOperation ||
            operation.Type is IPointerTypeSymbol or IFunctionPointerTypeSymbol)
        {
            return Unsupported(
                "ASCS4003",
                "Unsafe pointer and function-pointer semantics are not supported by the current AvidScript semantic profile.");
        }

        return hasStableProjection
            ? new SemanticSupportDecision(true, null, null)
            : Unsupported(
                "ASCS2001",
                $"The C# operation '{stableKind}' is not supported by the AvidScript semantic profile.");
    }

    public static SemanticSupportProjection ProjectDocument(SemanticCompilationContext context)
    {
        SemanticModel semanticModel = context.Compilation.GetSemanticModel(
            context.SyntaxTree,
            ignoreAccessibility: false);
        SyntaxNode root = context.SyntaxTree.GetRoot();
        List<SemanticDiagnostic> diagnostics = new();

        foreach (AnonymousFunctionExpressionSyntax lambda in root.DescendantNodes()
            .OfType<AnonymousFunctionExpressionSyntax>())
        {
            diagnostics.Add(CreateDiagnostic(
                "ASCS4001",
                "Lambda and closure syntax is not supported by the current AvidScript semantic profile.",
                context,
                lambda.Span));
        }

        foreach (LocalFunctionStatementSyntax localFunction in root.DescendantNodes()
            .OfType<LocalFunctionStatementSyntax>())
        {
            diagnostics.Add(CreateDiagnostic(
                "ASCS4001",
                "Local functions are not supported by the current AvidScript semantic profile.",
                context,
                localFunction.Span));
        }

        foreach (TypeSyntax typeSyntax in root.DescendantNodes().OfType<TypeSyntax>())
        {
            if (semanticModel.GetTypeInfo(typeSyntax).Type is IDynamicTypeSymbol)
            {
                diagnostics.Add(CreateDiagnostic(
                    "ASCS4002",
                    "Dynamic types are not supported by the current AvidScript semantic profile.",
                    context,
                    typeSyntax.Span));
            }
        }

        IEnumerable<SyntaxNode> unsafeSyntax = root.DescendantNodes().Where(node =>
            node is PointerTypeSyntax or FunctionPointerTypeSyntax or StackAllocArrayCreationExpressionSyntax);
        foreach (SyntaxNode syntax in unsafeSyntax)
        {
            diagnostics.Add(CreateDiagnostic(
                "ASCS4003",
                "Unsafe pointer, function-pointer, and stackalloc syntax is not supported by the current AvidScript semantic profile.",
                context,
                syntax.Span));
        }

        foreach (SyntaxToken unsafeToken in root.DescendantTokens().Where(token =>
            token.IsKind(SyntaxKind.UnsafeKeyword)))
        {
            diagnostics.Add(CreateDiagnostic(
                "ASCS4003",
                "Unsafe declarations and blocks are not supported by the current AvidScript semantic profile.",
                context,
                unsafeToken.Span));
        }

        return new SemanticSupportProjection(diagnostics
            .GroupBy(diagnostic => (diagnostic.Code, diagnostic.Span.Start, diagnostic.Span.Length))
            .Select(group => group.First())
            .OrderBy(diagnostic => diagnostic.Span.Start)
            .ThenBy(diagnostic => diagnostic.Code, StringComparer.Ordinal)
            .ToArray());
    }

    private static SemanticSupportDecision Unsupported(string code, string message)
    {
        return new SemanticSupportDecision(false, code, message);
    }

    private static SemanticDiagnostic CreateDiagnostic(
        string code,
        string message,
        SemanticCompilationContext context,
        Microsoft.CodeAnalysis.Text.TextSpan span)
    {
        return new SemanticDiagnostic(
            code,
            "error",
            message,
            SemanticSpanFactory.Create(context.SourceText, span));
    }
}
