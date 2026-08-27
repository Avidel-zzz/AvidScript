using System;
using System.Collections.Generic;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Operations;

namespace AvidScript.CSharpSemantic;

internal sealed record SemanticContinuationProjection(
    IReadOnlyList<SemanticContinuationCallback> Callbacks,
    IReadOnlyList<SemanticDiagnostic> Diagnostics);

internal static class SemanticContinuationProjector
{
    private const string ContinuationAttributeName =
        "global::AvidScript.AvidContinuationAttribute";
    private const string ContinuationsTypeName =
        "global::AvidScript.AvidContinuations";

    public static SemanticContinuationProjection Project(SemanticCompilationContext context)
    {
        SemanticModel semanticModel = context.Compilation.GetSemanticModel(
            context.PrimaryUnit.SyntaxTree,
            ignoreAccessibility: false);
        List<SemanticDiagnostic> diagnostics = new();
        List<SemanticContinuationCallback> callbacks = new();
        HashSet<int> callbackIds = new();

        MethodDeclarationSyntax[] handlers = context.PrimaryUnit.SyntaxTree
            .GetRoot()
            .DescendantNodes()
            .OfType<MethodDeclarationSyntax>()
            .Where(declaration => semanticModel.GetDeclaredSymbol(declaration) is IMethodSymbol method
                && FindAttribute(method.GetAttributes()) is not null)
            .OrderBy(declaration => declaration.SpanStart)
            .ToArray();

        foreach (MethodDeclarationSyntax declaration in handlers)
        {
            if (semanticModel.GetDeclaredSymbol(declaration) is not IMethodSymbol method)
            {
                continue;
            }

            SemanticSpan span = SemanticSpanFactory.Create(
                context.PrimaryUnit.SourceText,
                declaration.Identifier.Span);
            AttributeData attribute = FindAttribute(method.GetAttributes())!;
            int? callbackId = ReadCallbackId(attribute);
            bool valid = true;
            if (callbackId is null || callbackId <= 0)
            {
                diagnostics.Add(Error(
                    "ASCS5301",
                    $"Continuation handler '{method.Name}' must declare a positive constant callback id.",
                    span));
                valid = false;
            }
            else if (!callbackIds.Add(callbackId.Value))
            {
                diagnostics.Add(Error(
                    "ASCS5302",
                    $"Continuation callback id '{callbackId.Value}' has more than one handler.",
                    span));
                valid = false;
            }

            if (method.DeclaredAccessibility != Accessibility.Public
                || !method.IsStatic
                || !method.ReturnsVoid
                || method.Parameters.Length != 0
                || method.IsGenericMethod
                || method.ContainingType.IsGenericType
                || method.IsAbstract
                || method.IsExtern
                || declaration.Body is null && declaration.ExpressionBody is null)
            {
                diagnostics.Add(Error(
                    "ASCS5303",
                    $"Continuation handler '{method.Name}' must be public static concrete non-generic void with zero parameters.",
                    span));
                valid = false;
            }

            if (valid)
            {
                callbacks.Add(new SemanticContinuationCallback(
                    callbackId!.Value,
                    method.Name,
                    SemanticSymbolProjector.GetSymbolId(method),
                    span));
            }
        }

        foreach (LocalFunctionStatementSyntax declaration in context.PrimaryUnit.SyntaxTree
            .GetRoot()
            .DescendantNodes()
            .OfType<LocalFunctionStatementSyntax>()
            .OrderBy(node => node.SpanStart))
        {
            if (semanticModel.GetDeclaredSymbol(declaration) is IMethodSymbol method
                && FindAttribute(method.GetAttributes()) is not null)
            {
                diagnostics.Add(Error(
                    "ASCS5303",
                    $"Continuation handler '{method.Name}' must be a public static member method.",
                    SemanticSpanFactory.Create(
                        context.PrimaryUnit.SourceText,
                        declaration.Identifier.Span)));
            }
        }

        IReadOnlySet<int> declaredCallbackIds = callbacks
            .Select(callback => callback.CallbackId)
            .ToHashSet();
        foreach (InvocationExpressionSyntax invocation in context.PrimaryUnit.SyntaxTree
            .GetRoot()
            .DescendantNodes()
            .OfType<InvocationExpressionSyntax>()
            .OrderBy(node => node.SpanStart))
        {
            if (semanticModel.GetOperation(invocation) is not IInvocationOperation operation
                || !IsContinuationCall(operation.TargetMethod))
            {
                continue;
            }

            IArgumentOperation? callbackArgument = operation.Arguments.FirstOrDefault(argument =>
                string.Equals(argument.Parameter?.Name, "callbackId", StringComparison.Ordinal));
            SemanticSpan span = SemanticSpanFactory.Create(
                context.PrimaryUnit.SourceText,
                callbackArgument?.Syntax.Span ?? invocation.Span);
            int? callbackId = null;
            if (callbackArgument is not null
                && callbackArgument.Value.ConstantValue.HasValue
                && callbackArgument.Value.ConstantValue.Value is int value)
            {
                callbackId = value;
            }
            if (callbackId is null || callbackId <= 0)
            {
                diagnostics.Add(Error(
                    "ASCS5304",
                    $"AvidContinuations.{operation.TargetMethod.Name} requires a compile-time positive callback id.",
                    span));
            }
            else if (!declaredCallbackIds.Contains(callbackId.Value))
            {
                diagnostics.Add(Error(
                    "ASCS5305",
                    $"AvidContinuations.{operation.TargetMethod.Name} callback id '{callbackId.Value}' has no declared handler.",
                    span));
            }
        }

        if (diagnostics.Count != 0)
        {
            callbacks.Clear();
        }

        return new SemanticContinuationProjection(
            callbacks.OrderBy(callback => callback.CallbackId).ToArray(),
            diagnostics
                .OrderBy(diagnostic => diagnostic.Span.Start)
                .ThenBy(diagnostic => diagnostic.Code, StringComparer.Ordinal)
                .ToArray());
    }

    private static AttributeData? FindAttribute(IEnumerable<AttributeData> attributes)
    {
        return attributes.FirstOrDefault(attribute => string.Equals(
            attribute.AttributeClass?.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat),
            ContinuationAttributeName,
            StringComparison.Ordinal));
    }

    private static int? ReadCallbackId(AttributeData attribute)
    {
        return attribute.ConstructorArguments.Length == 1
            && attribute.ConstructorArguments[0].Kind == TypedConstantKind.Primitive
            && attribute.ConstructorArguments[0].Value is int callbackId
                ? callbackId
                : null;
    }

    private static bool IsContinuationCall(IMethodSymbol method)
    {
        return method.Name is "Delay" or "NextTick"
            && string.Equals(
                method.ContainingType.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat),
                ContinuationsTypeName,
                StringComparison.Ordinal);
    }

    private static SemanticDiagnostic Error(string code, string message, SemanticSpan span)
    {
        return new SemanticDiagnostic(code, "error", message, span);
    }
}
