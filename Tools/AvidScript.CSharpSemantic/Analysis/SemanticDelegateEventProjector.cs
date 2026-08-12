using System;
using System.Collections.Generic;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace AvidScript.CSharpSemantic;

internal sealed record SemanticDelegateEventProjection(
    IReadOnlyList<SemanticDelegateEventCallback> Callbacks,
    IReadOnlyList<SemanticDiagnostic> Diagnostics);

internal static class SemanticDelegateEventProjector
{
    private const string EventAttributeName = "global::AvidScript.AvidEventAttribute";
    private const string ContractAttributeName = "global::AvidScript.AvidEventContractAttribute";
    private const string ExportPrefix = "avid_on_delegate_";

    private sealed record EventContract(string SubscriptionId, IReadOnlyList<string> ParameterTypes);

    public static SemanticDelegateEventProjection Project(SemanticCompilationContext context)
    {
        List<SemanticDiagnostic> diagnostics = new();
        Dictionary<string, EventContract> contracts = CollectContracts(context, diagnostics);
        SemanticModel semanticModel = context.Compilation.GetSemanticModel(
            context.PrimaryUnit.SyntaxTree,
            ignoreAccessibility: false);
        List<SemanticDelegateEventCallback> callbacks = new();
        HashSet<string> observedSubscriptions = new(StringComparer.Ordinal);
        HashSet<string> observedExports = new(StringComparer.Ordinal);

        MethodDeclarationSyntax[] candidates = context.PrimaryUnit.SyntaxTree
            .GetRoot()
            .DescendantNodes()
            .OfType<MethodDeclarationSyntax>()
            .Where(declaration => semanticModel.GetDeclaredSymbol(declaration) is IMethodSymbol method
                && FindAttribute(method.GetAttributes(), EventAttributeName) is not null)
            .OrderBy(declaration => declaration.SpanStart)
            .ToArray();

        foreach (MethodDeclarationSyntax declaration in candidates)
        {
            if (semanticModel.GetDeclaredSymbol(declaration) is not IMethodSymbol method)
            {
                continue;
            }

            SemanticSpan span = SemanticSpanFactory.Create(
                context.PrimaryUnit.SourceText,
                declaration.Identifier.Span);
            AttributeData attribute = FindAttribute(method.GetAttributes(), EventAttributeName)!;
            string? subscriptionId = ReadStringArgument(attribute, 0);
            bool valid = true;
            if (subscriptionId is null || !IsStableId(subscriptionId))
            {
                diagnostics.Add(Error(
                    "ASCS5203",
                    $"Delegate event handler '{method.Name}' must reference one valid generated subscription id.",
                    span));
                valid = false;
            }

            EventContract? contract = null;
            if (subscriptionId is not null && !contracts.TryGetValue(subscriptionId, out contract))
            {
                diagnostics.Add(Error(
                    "ASCS5203",
                    $"Delegate event handler '{method.Name}' references an unknown subscription contract.",
                    span));
                valid = false;
            }

            if (method.DeclaredAccessibility != Accessibility.Public
                || !method.IsStatic
                || !method.ReturnsVoid)
            {
                diagnostics.Add(Error(
                    "ASCS5204",
                    $"Delegate event handler '{method.Name}' must be public static and return void.",
                    span));
                valid = false;
            }

            if (method.IsGenericMethod
                || method.IsExtern
                || method.IsAbstract
                || declaration.Body is null && declaration.ExpressionBody is null
                || method.Parameters.Any(parameter => parameter.RefKind != RefKind.None
                    || parameter.HasExplicitDefaultValue))
            {
                diagnostics.Add(Error(
                    "ASCS5205",
                    $"Delegate event handler '{method.Name}' must be concrete, non-generic, and cannot use ref, out, in, or default parameters.",
                    span));
                valid = false;
            }

            if (contract is not null && !ParametersMatch(method.Parameters, contract.ParameterTypes))
            {
                diagnostics.Add(Error(
                    "ASCS5206",
                    $"Delegate event handler '{method.Name}' does not exactly match its generated parameter contract.",
                    span));
                valid = false;
            }

            if (subscriptionId is not null && !observedSubscriptions.Add(subscriptionId))
            {
                diagnostics.Add(Error(
                    "ASCS5207",
                    $"Delegate subscription '{subscriptionId}' has more than one handler.",
                    span));
                valid = false;
            }

            string? exportName = subscriptionId is not null && IsStableId(subscriptionId)
                ? ExportPrefix + subscriptionId[..16]
                : null;
            if (exportName is not null && !observedExports.Add(exportName))
            {
                diagnostics.Add(Error(
                    "ASCS5208",
                    $"Delegate subscription '{subscriptionId}' collides with another generated export name.",
                    span));
                valid = false;
            }

            if (valid)
            {
                callbacks.Add(new SemanticDelegateEventCallback(
                    subscriptionId!,
                    exportName!,
                    method.Name,
                    SemanticSymbolProjector.GetSymbolId(method),
                    span));
            }
        }

        if (diagnostics.Count != 0)
        {
            callbacks.Clear();
        }

        return new SemanticDelegateEventProjection(
            callbacks.OrderBy(callback => callback.SubscriptionId, StringComparer.Ordinal).ToArray(),
            diagnostics
                .OrderBy(diagnostic => diagnostic.Span.Start)
                .ThenBy(diagnostic => diagnostic.Code, StringComparer.Ordinal)
                .ToArray());
    }

    private static Dictionary<string, EventContract> CollectContracts(
        SemanticCompilationContext context,
        List<SemanticDiagnostic> diagnostics)
    {
        Dictionary<string, EventContract> contracts = new(StringComparer.Ordinal);
        foreach (SyntaxTree syntaxTree in context.Compilation.SyntaxTrees)
        {
            SemanticModel model = context.Compilation.GetSemanticModel(syntaxTree, ignoreAccessibility: false);
            foreach (VariableDeclaratorSyntax declaration in syntaxTree.GetRoot()
                .DescendantNodes()
                .OfType<VariableDeclaratorSyntax>())
            {
                if (model.GetDeclaredSymbol(declaration) is not IFieldSymbol field
                    || FindAttribute(field.GetAttributes(), ContractAttributeName) is not { } attribute)
                {
                    continue;
                }

                SemanticSpan span = syntaxTree == context.PrimaryUnit.SyntaxTree
                    ? SemanticSpanFactory.Create(context.PrimaryUnit.SourceText, declaration.Identifier.Span)
                    : SemanticSpanFactory.Empty;
                string? subscriptionId = ReadStringArgument(attribute, 0);
                string? parameterTypes = ReadStringArgument(attribute, 1);
                string[] parameters = parameterTypes is null || parameterTypes.Length == 0
                    ? Array.Empty<string>()
                    : parameterTypes.Split(';', StringSplitOptions.None);
                bool valid = subscriptionId is not null
                    && IsStableId(subscriptionId)
                    && field.IsConst
                    && field.Type.SpecialType == SpecialType.System_String
                    && field.ConstantValue is string value
                    && string.Equals(value, subscriptionId, StringComparison.Ordinal)
                    && parameterTypes is not null
                    && parameters.All(parameter => !string.IsNullOrWhiteSpace(parameter)
                        && string.Equals(parameter, parameter.Trim(), StringComparison.Ordinal));
                if (!valid)
                {
                    diagnostics.Add(Error(
                        "ASCS5201",
                        $"Delegate event contract '{field.Name}' is malformed.",
                        span));
                    continue;
                }

                EventContract contract = new(subscriptionId!, parameters);
                if (!contracts.TryAdd(subscriptionId!, contract))
                {
                    diagnostics.Add(Error(
                        "ASCS5202",
                        $"Delegate event contract '{subscriptionId}' is declared more than once.",
                        span));
                }
            }
        }

        return contracts;
    }

    private static AttributeData? FindAttribute(
        IEnumerable<AttributeData> attributes,
        string fullyQualifiedName)
    {
        return attributes.FirstOrDefault(attribute => string.Equals(
            attribute.AttributeClass?.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat),
            fullyQualifiedName,
            StringComparison.Ordinal));
    }

    private static string? ReadStringArgument(AttributeData attribute, int ordinal)
    {
        return attribute.ConstructorArguments.Length > ordinal
            && attribute.ConstructorArguments[ordinal].Kind == TypedConstantKind.Primitive
            ? attribute.ConstructorArguments[ordinal].Value as string
            : null;
    }

    private static bool ParametersMatch(
        IReadOnlyList<IParameterSymbol> parameters,
        IReadOnlyList<string> expectedTypes)
    {
        return parameters.Count == expectedTypes.Count
            && parameters.Select((parameter, index) => parameter.RefKind == RefKind.None
                && string.Equals(
                    parameter.Type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat),
                    expectedTypes[index],
                    StringComparison.Ordinal))
                .All(matches => matches);
    }

    private static bool IsStableId(string value)
    {
        return value.Length == 64
            && value.All(character => character is >= '0' and <= '9' or >= 'a' and <= 'f');
    }

    private static SemanticDiagnostic Error(string code, string message, SemanticSpan span)
    {
        return new SemanticDiagnostic(code, "error", message, span);
    }
}
