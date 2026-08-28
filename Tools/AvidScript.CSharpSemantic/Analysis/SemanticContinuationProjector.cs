using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
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
    private const string AssetsTypeName =
        "global::AvidScript.AvidAssets";
    private const string ContinuationStatusTypeName =
        "global::AvidScript.AvidContinuationStatus";
    private const string LoadedObjectTypeName =
        "global::AvidScript.AvidLoadedObject";
    private const int MaximumAssetPathUtf8Bytes = 1024;

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

            string? payloadKind = GetPayloadKind(method);
            if (method.DeclaredAccessibility != Accessibility.Public
                || !method.IsStatic
                || !method.ReturnsVoid
                || payloadKind is null
                || method.IsGenericMethod
                || method.ContainingType.IsGenericType
                || method.IsAbstract
                || method.IsExtern
                || declaration.Body is null && declaration.ExpressionBody is null)
            {
                diagnostics.Add(Error(
                    "ASCS5303",
                    $"Continuation handler '{method.Name}' must be public static concrete non-generic void with either zero parameters or exact (AvidContinuationStatus, AvidLoadedObject) parameters.",
                    span));
                valid = false;
            }

            if (valid)
            {
                callbacks.Add(new SemanticContinuationCallback(
                    callbackId!.Value,
                    method.Name,
                    SemanticSymbolProjector.GetSymbolId(method),
                    span)
                {
                    PayloadKind = payloadKind!,
                });
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

        IReadOnlyDictionary<int, SemanticContinuationCallback> declaredCallbacks = callbacks
            .ToDictionary(callback => callback.CallbackId);
        foreach (InvocationExpressionSyntax invocation in context.PrimaryUnit.SyntaxTree
            .GetRoot()
            .DescendantNodes()
            .OfType<InvocationExpressionSyntax>()
            .OrderBy(node => node.SpanStart))
        {
            if (semanticModel.GetOperation(invocation) is not IInvocationOperation operation)
            {
                continue;
            }

            bool isContinuationCall = IsContinuationCall(operation.TargetMethod);
            bool isObjectLoadCall = IsObjectLoadCall(operation.TargetMethod);
            if (!isContinuationCall && !isObjectLoadCall)
            {
                continue;
            }

            if (isObjectLoadCall)
            {
                ValidateAssetPath(context, operation, invocation, diagnostics);
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
            string ownerName = isObjectLoadCall ? "AvidAssets" : "AvidContinuations";
            if (callbackId is null || callbackId <= 0)
            {
                diagnostics.Add(Error(
                    "ASCS5304",
                    $"{ownerName}.{operation.TargetMethod.Name} requires a compile-time positive callback id.",
                    span));
            }
            else if (!declaredCallbacks.TryGetValue(
                callbackId.Value,
                out SemanticContinuationCallback? callback))
            {
                diagnostics.Add(Error(
                    "ASCS5305",
                    $"{ownerName}.{operation.TargetMethod.Name} callback id '{callbackId.Value}' has no declared handler.",
                    span));
            }
            else if (isObjectLoadCall
                && callback.PayloadKind != SemanticContinuationCallback.ObjectPayloadKind)
            {
                diagnostics.Add(Error(
                    "ASCS5309",
                    $"AvidAssets.LoadObjectAsync callback id '{callbackId.Value}' requires an object-payload continuation handler.",
                    span));
            }
            else if (isContinuationCall
                && callback.PayloadKind != SemanticContinuationCallback.NonePayloadKind)
            {
                diagnostics.Add(Error(
                    "ASCS5310",
                    $"AvidContinuations.{operation.TargetMethod.Name} callback id '{callbackId.Value}' requires a zero-parameter continuation handler.",
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

    private static bool IsObjectLoadCall(IMethodSymbol method)
    {
        return method.Name == "LoadObjectAsync"
            && string.Equals(
                method.ContainingType.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat),
                AssetsTypeName,
                StringComparison.Ordinal);
    }

    private static string? GetPayloadKind(IMethodSymbol method)
    {
        if (method.Parameters.Length == 0)
        {
            return SemanticContinuationCallback.NonePayloadKind;
        }

        return method.Parameters.Length == 2
            && method.Parameters.All(parameter => parameter.RefKind == RefKind.None)
            && string.Equals(
                method.Parameters[0].Type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat),
                ContinuationStatusTypeName,
                StringComparison.Ordinal)
            && string.Equals(
                method.Parameters[1].Type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat),
                LoadedObjectTypeName,
                StringComparison.Ordinal)
                ? SemanticContinuationCallback.ObjectPayloadKind
                : null;
    }

    private static void ValidateAssetPath(
        SemanticCompilationContext context,
        IInvocationOperation operation,
        InvocationExpressionSyntax invocation,
        ICollection<SemanticDiagnostic> diagnostics)
    {
        IArgumentOperation? assetPathArgument = operation.Arguments.FirstOrDefault(argument =>
            string.Equals(argument.Parameter?.Name, "assetPath", StringComparison.Ordinal));
        SemanticSpan span = SemanticSpanFactory.Create(
            context.PrimaryUnit.SourceText,
            assetPathArgument?.Syntax.Span ?? invocation.Span);
        if (assetPathArgument?.Value.ConstantValue is not { HasValue: true, Value: string assetPath }
            || assetPath.Length == 0)
        {
            diagnostics.Add(Error(
                "ASCS5306",
                "AvidAssets.LoadObjectAsync requires a compile-time nonempty asset path.",
                span));
            return;
        }

        if (assetPath.IndexOf('\0') >= 0 || !IsCanonicalLookingAssetPath(assetPath))
        {
            diagnostics.Add(Error(
                "ASCS5307",
                "AvidAssets.LoadObjectAsync asset path must be canonical-looking and contain no NUL character.",
                span));
        }

        if (Encoding.UTF8.GetByteCount(assetPath) > MaximumAssetPathUtf8Bytes)
        {
            diagnostics.Add(Error(
                "ASCS5308",
                $"AvidAssets.LoadObjectAsync asset path must not exceed {MaximumAssetPathUtf8Bytes} UTF-8 bytes.",
                span));
        }
    }

    private static bool IsCanonicalLookingAssetPath(string assetPath)
    {
        if (assetPath.Length < 4
            || assetPath[0] != '/'
            || assetPath[1] == '/'
            || assetPath.IndexOf('\\') >= 0
            || assetPath.Any(character => char.IsControl(character) || char.IsWhiteSpace(character)))
        {
            return false;
        }

        int lastSlash = assetPath.LastIndexOf('/');
        int objectSeparator = assetPath.IndexOf('.', lastSlash + 1);
        return lastSlash > 0
            && objectSeparator > lastSlash + 1
            && objectSeparator < assetPath.Length - 1
            && assetPath.IndexOf('.', objectSeparator + 1) < 0
            && !assetPath.Contains("//", StringComparison.Ordinal);
    }

    private static SemanticDiagnostic Error(string code, string message, SemanticSpan span)
    {
        return new SemanticDiagnostic(code, "error", message, span);
    }
}
