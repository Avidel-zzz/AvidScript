using System;
using System.Collections.Generic;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace AvidScript.CSharpSemantic;

internal sealed record SemanticGameplayEventProjection(
    IReadOnlyList<SemanticGameplayEventCallback> Callbacks,
    IReadOnlyList<SemanticDiagnostic> Diagnostics);

internal static class SemanticGameplayEventProjector
{
    private sealed record CallbackContract(
        int EventType,
        string Name,
        IReadOnlyList<string> ParameterTypes);

    private static readonly CallbackContract[] Contracts =
    {
        new(1, "OnBeginOverlap", new[] { "global::AvidScript.AActor", "global::AvidScript.FVector" }),
        new(2, "OnEndOverlap", new[] { "global::AvidScript.AActor", "global::AvidScript.FVector" }),
        new(3, "OnHit", new[] { "global::AvidScript.AActor", "global::AvidScript.FVector" }),
        new(4, "OnInput", new[] { "global::AvidScript.InputEvent" }),
    };

    public static SemanticGameplayEventProjection Project(
        SemanticCompilationContext context,
        IReadOnlyList<SemanticCallable> callables)
    {
        Dictionary<string, CallbackContract> contractsByName = Contracts.ToDictionary(
            contract => contract.Name,
            StringComparer.Ordinal);
        SemanticModel semanticModel = context.Compilation.GetSemanticModel(
            context.PrimaryUnit.SyntaxTree,
            ignoreAccessibility: false);
        List<SemanticGameplayEventCallback> callbacks = new();
        List<SemanticDiagnostic> diagnostics = new();
        HashSet<int> observedEventTypes = new();
        MethodDeclarationSyntax[] candidates = context.PrimaryUnit.SyntaxTree
            .GetRoot()
            .DescendantNodes()
            .OfType<MethodDeclarationSyntax>()
            .Where(method => contractsByName.ContainsKey(method.Identifier.ValueText))
            .OrderBy(method => method.SpanStart)
            .ToArray();
        HashSet<string> exportedOwnerIds = callables
            .Where(callable => callable.Export is not null)
            .Select(callable => callable.ContainingTypeId)
            .ToHashSet(StringComparer.Ordinal);
        HashSet<string> candidateOwnerIds = candidates
            .Select(declaration => semanticModel.GetDeclaredSymbol(declaration))
            .OfType<IMethodSymbol>()
            .Select(method => "type:" + SemanticTypeRegistry.GetCanonicalName(method.ContainingType))
            .ToHashSet(StringComparer.Ordinal);
        IReadOnlySet<string> scriptOwnerIds = exportedOwnerIds.Count == 0
            ? candidateOwnerIds
            : exportedOwnerIds;
        if (exportedOwnerIds.Count == 0 && candidateOwnerIds.Count > 1)
        {
            MethodDeclarationSyntax first = candidates[0];
            diagnostics.Add(CreateDiagnostic(
                "ASCS5108",
                "Gameplay callbacks require one unambiguous script host type when no direct ABI export exists.",
                SemanticSpanFactory.Create(context.PrimaryUnit.SourceText, first.Identifier.Span)));
        }

        foreach (MethodDeclarationSyntax declaration in candidates)
        {
            CallbackContract contract = contractsByName[declaration.Identifier.ValueText];
            if (semanticModel.GetDeclaredSymbol(declaration) is not IMethodSymbol method)
            {
                continue;
            }

            string ownerId = "type:" + SemanticTypeRegistry.GetCanonicalName(method.ContainingType);
            if (!scriptOwnerIds.Contains(ownerId))
            {
                continue;
            }

            SemanticSpan span = SemanticSpanFactory.Create(
                context.PrimaryUnit.SourceText,
                declaration.Identifier.Span);
            bool valid = true;
            if (method.DeclaredAccessibility != Accessibility.Public)
            {
                diagnostics.Add(CreateDiagnostic(
                    "ASCS5101",
                    $"Gameplay callback '{contract.Name}' must be public.",
                    span));
                valid = false;
            }

            if (!method.IsStatic)
            {
                diagnostics.Add(CreateDiagnostic(
                    "ASCS5102",
                    $"Gameplay callback '{contract.Name}' must be static.",
                    span));
                valid = false;
            }

            if (!method.ReturnsVoid)
            {
                diagnostics.Add(CreateDiagnostic(
                    "ASCS5103",
                    $"Gameplay callback '{contract.Name}' must return void.",
                    span));
                valid = false;
            }

            if (method.IsGenericMethod
                || method.IsExtern
                || method.IsAbstract
                || (declaration.Body is null && declaration.ExpressionBody is null))
            {
                diagnostics.Add(CreateDiagnostic(
                    "ASCS5107",
                    $"Gameplay callback '{contract.Name}' must have one concrete non-generic implementation.",
                    span));
                valid = false;
            }

            if (method.Parameters.Length != contract.ParameterTypes.Count)
            {
                diagnostics.Add(CreateDiagnostic(
                    "ASCS5104",
                    $"Gameplay callback '{contract.Name}' has an invalid parameter count.",
                    span));
                valid = false;
            }
            else if (!ParametersMatch(method.Parameters, contract.ParameterTypes))
            {
                diagnostics.Add(CreateDiagnostic(
                    "ASCS5105",
                    $"Gameplay callback '{contract.Name}' has an invalid parameter type or ref kind.",
                    span));
                valid = false;
            }

            if (!observedEventTypes.Add(contract.EventType))
            {
                diagnostics.Add(CreateDiagnostic(
                    "ASCS5106",
                    $"Gameplay callback '{contract.Name}' is declared more than once.",
                    span));
                valid = false;
            }

            if (valid)
            {
                callbacks.Add(new SemanticGameplayEventCallback(
                    contract.EventType,
                    contract.Name,
                    SemanticSymbolProjector.GetSymbolId(method),
                    span));
            }
        }

        if (diagnostics.Count != 0)
        {
            callbacks.Clear();
        }

        return new SemanticGameplayEventProjection(
            callbacks.OrderBy(callback => callback.EventType).ToArray(),
            diagnostics
                .OrderBy(diagnostic => diagnostic.Span.Start)
                .ThenBy(diagnostic => diagnostic.Code, StringComparer.Ordinal)
                .ToArray());
    }

    private static bool ParametersMatch(
        IReadOnlyList<IParameterSymbol> parameters,
        IReadOnlyList<string> expectedTypes)
    {
        for (int index = 0; index < parameters.Count; ++index)
        {
            IParameterSymbol parameter = parameters[index];
            if (parameter.RefKind != RefKind.None
                || !string.Equals(
                    parameter.Type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat),
                    expectedTypes[index],
                    StringComparison.Ordinal))
            {
                return false;
            }
        }

        return true;
    }

    private static SemanticDiagnostic CreateDiagnostic(
        string code,
        string message,
        SemanticSpan span)
    {
        return new SemanticDiagnostic(code, "error", message, span);
    }
}
