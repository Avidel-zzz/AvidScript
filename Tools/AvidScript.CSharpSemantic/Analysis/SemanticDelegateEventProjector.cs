using System;
using System.Collections.Generic;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Operations;

namespace AvidScript.CSharpSemantic;

internal sealed record SemanticDelegateEventProjection(
    IReadOnlyList<SemanticDelegateEventCallback> Callbacks,
    IReadOnlyList<SemanticDiagnostic> Diagnostics,
    IReadOnlyList<SemanticEventSubscription> Subscriptions);

internal static class SemanticDelegateEventProjector
{
    private const string EventAttributeName = "global::AvidScript.AvidEventAttribute";
    private const string ContractAttributeName = "global::AvidScript.AvidEventContractAttribute";
    private const string LanguageAttributeName = "global::AvidScript.AvidEventLanguageAttribute";
    private const string ExportPrefix = "avid_on_delegate_";

    private sealed record EventContract(
        string SubscriptionId,
        IReadOnlyList<string> ParameterTypes,
        IReadOnlyList<string> ParameterDirections,
        string ReturnType);

    public static SemanticDelegateEventProjection Project(SemanticCompilationContext context, SemanticTypeRegistry typeRegistry)
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
                || !method.IsStatic)
            {
                diagnostics.Add(Error(
                    "ASCS5204",
                    $"Delegate event handler '{method.Name}' must be public static.",
                    span));
                valid = false;
            }

            if (method.IsGenericMethod
                || method.IsExtern
                || method.IsAbstract
                || declaration.Body is null && declaration.ExpressionBody is null
                || method.Parameters.Any(parameter => parameter.RefKind == RefKind.In
                    || parameter.HasExplicitDefaultValue))
            {
                diagnostics.Add(Error(
                    "ASCS5205",
                    $"Delegate event handler '{method.Name}' must be concrete, non-generic, and cannot use in or default parameters.",
                    span));
                valid = false;
            }

            if (contract is not null
                && (!ParametersMatch(
                    method.Parameters,
                    contract.ParameterTypes,
                    contract.ParameterDirections)
                    || !TypeMatches(method.ReturnType, contract.ReturnType)))
            {
                diagnostics.Add(Error(
                    "ASCS5206",
                    $"Delegate event handler '{method.Name}' does not exactly match its generated parameter and return contract.",
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

        IReadOnlyList<SemanticEventSubscription> subscriptions = CollectSubscriptions(context, typeRegistry, contracts, diagnostics);
        return new SemanticDelegateEventProjection(
            (diagnostics.Count == 0 ? callbacks : new()).OrderBy(callback => callback.SubscriptionId, StringComparer.Ordinal).ToArray(),
            diagnostics
                .OrderBy(diagnostic => diagnostic.Span.Start)
                .ThenBy(diagnostic => diagnostic.Code, StringComparer.Ordinal)
                .ToArray(), subscriptions);
    }

    private static IReadOnlyList<SemanticEventSubscription> CollectSubscriptions(
        SemanticCompilationContext context, SemanticTypeRegistry registry,
        IReadOnlyDictionary<string, EventContract> contracts, List<SemanticDiagnostic> diagnostics)
    {
        List<SemanticEventSubscription> result = new();
        HashSet<string> ids = new(StringComparer.Ordinal), exports = new(StringComparer.Ordinal);
        HashSet<int> ordinals = new();
        foreach (SyntaxTree tree in context.Compilation.SyntaxTrees)
        {
            SemanticModel model = context.Compilation.GetSemanticModel(tree);
            foreach (MethodDeclarationSyntax syntax in tree.GetRoot().DescendantNodes().OfType<MethodDeclarationSyntax>())
            {
                if (model.GetDeclaredSymbol(syntax) is not IMethodSymbol method
                    || FindAttribute(method.GetAttributes(), "global::AvidScript.AvidEventSubscriptionAttribute") is not { } attribute) continue;
                string? id = ReadStringArgument(attribute, 0);
                int ordinal = attribute.ConstructorArguments.Length == 2 && attribute.ConstructorArguments[1].Value is int value ? value : -1;
                IMethodSymbol? invoke = method.Parameters.Length == 3
                    ? (method.Parameters[2].Type as INamedTypeSymbol)?.DelegateInvokeMethod : null;
                bool valid = id is not null && IsStableId(id) && contracts.TryGetValue(id, out EventContract? contract)
                    && ordinal >= 0 && ordinal < int.MaxValue && method.IsStatic && method.IsExtern && !method.IsGenericMethod
                    && method.ContainingType.Arity == 0 && syntax.Body is null && syntax.ExpressionBody is null
                    && method.GetDllImportData() is null && method.ReturnType.SpecialType == SpecialType.System_Int64
                    && method.RefKind == RefKind.None && method.Parameters.Length == 3
                    && method.Parameters.All(p => p.RefKind == RefKind.None && !p.HasExplicitDefaultValue && !p.IsParams)
                    && method.Parameters[0].Type.SpecialType == SpecialType.System_Int32
                    && method.Parameters[1].Type.SpecialType == SpecialType.System_Int32
                    && invoke is not null && invoke.RefKind == RefKind.None
                    && ParametersMatch(invoke.Parameters, contract.ParameterTypes, contract.ParameterDirections)
                    && TypeMatches(invoke.ReturnType, contract.ReturnType)
                    && ids.Add(id) && exports.Add(ExportPrefix + id[..16]) && ordinals.Add(ordinal);
                if (!valid)
                {
                    diagnostics.Add(Error("ASCS5209", $"Typed event subscription '{method.Name}' must uniquely match a generated event and expose static extern long(int, int, exact delegate).",
                        tree == context.PrimaryUnit.SyntaxTree ? SemanticSpanFactory.Create(context.PrimaryUnit.SourceText, syntax.Identifier.Span) : SemanticSpanFactory.Empty));
                    continue;
                }
                result.Add(new(id!, ordinal, SemanticSymbolProjector.GetSymbolId(method), registry.Register(method.Parameters[2].Type)));
            }
        }
        CollectLanguageEvents(context, registry, result, diagnostics);
        ValidateLanguageAssignments(context, result, diagnostics);
        return diagnostics.Count == 0 ? result.OrderBy(item => item.SubscriptionId, StringComparer.Ordinal).ToArray() : Array.Empty<SemanticEventSubscription>();
    }

    private static void CollectLanguageEvents(
        SemanticCompilationContext context, SemanticTypeRegistry registry,
        List<SemanticEventSubscription> subscriptions, List<SemanticDiagnostic> diagnostics)
    {
        Dictionary<string, int> subscriptionsById = subscriptions
            .Select((entry, index) => (entry.SubscriptionId, index))
            .ToDictionary(entry => entry.SubscriptionId, entry => entry.index, StringComparer.Ordinal);
        HashSet<string> observedIds = new(StringComparer.Ordinal);
        foreach (SyntaxTree tree in context.Compilation.SyntaxTrees)
        {
            SemanticModel model = context.Compilation.GetSemanticModel(tree);
            foreach (EventDeclarationSyntax syntax in tree.GetRoot().DescendantNodes().OfType<EventDeclarationSyntax>())
            {
                if (model.GetDeclaredSymbol(syntax) is not IEventSymbol eventSymbol
                    || FindAttribute(eventSymbol.GetAttributes(), LanguageAttributeName) is not { } attribute) continue;
                string? id = ReadStringArgument(attribute, 0);
                int ordinal = attribute.ConstructorArguments.Length == 2 && attribute.ConstructorArguments[1].Value is int value ? value : -1;
                bool generatedReference = tree != context.PrimaryUnit.SyntaxTree
                    && context.ProjectionUnits.Any(unit => unit.SyntaxTree == tree);
                bool emptyAccessors = syntax.AccessorList?.Accessors.Count == 2
                    && syntax.AccessorList.Accessors.All(accessor =>
                        accessor.Body?.Statements.Count == 0 && accessor.ExpressionBody is null)
                    && syntax.AccessorList.Accessors.Any(accessor => accessor.Keyword.ValueText == "add")
                    && syntax.AccessorList.Accessors.Any(accessor => accessor.Keyword.ValueText == "remove");
                int index = -1;
                bool valid = id is not null && IsStableId(id) && observedIds.Add(id)
                    && subscriptionsById.TryGetValue(id, out index)
                    && subscriptions[index].EventOrdinal == ordinal
                    && registry.Register(eventSymbol.Type) == subscriptions[index].DelegateTypeId
                    && eventSymbol.DeclaredAccessibility == Accessibility.Public && !eventSymbol.IsStatic
                    && eventSymbol.ContainingType.TypeKind == TypeKind.Struct && eventSymbol.ContainingType.IsReadOnly
                    && eventSymbol.ContainingType.Arity == 0
                    && HasHandleFields(eventSymbol.ContainingType)
                    && eventSymbol.AddMethod?.DeclaredAccessibility == Accessibility.Public
                    && eventSymbol.RemoveMethod?.DeclaredAccessibility == Accessibility.Public
                    && generatedReference && emptyAccessors;
                if (!valid)
                {
                    diagnostics.Add(Error("ASCS5210",
                        $"Language event '{eventSymbol.Name}' must be a generated public instance event with matching id, ordinal, delegate, and empty add/remove accessors.",
                        tree == context.PrimaryUnit.SyntaxTree
                            ? SemanticSpanFactory.Create(context.PrimaryUnit.SourceText, syntax.Identifier.Span)
                            : SemanticSpanFactory.Empty));
                    continue;
                }
                subscriptions[index] = subscriptions[index] with
                {
                    EventSymbolId = SemanticSymbolProjector.GetSymbolId(eventSymbol),
                    OwnerTypeId = registry.Register(eventSymbol.ContainingType),
                };
            }
        }
    }

    private static bool HasHandleFields(INamedTypeSymbol owner)
    {
        return new[] { "Slot", "Generation" }.All(name =>
            owner.GetMembers(name).OfType<IFieldSymbol>().Count(field =>
                !field.IsStatic && field.IsReadOnly && field.Type.SpecialType == SpecialType.System_Int32) == 1);
    }

    private static void ValidateLanguageAssignments(
        SemanticCompilationContext context, IReadOnlyList<SemanticEventSubscription> subscriptions,
        List<SemanticDiagnostic> diagnostics)
    {
        HashSet<string> generatedEvents = subscriptions.Where(entry => entry.EventSymbolId is not null)
            .Select(entry => entry.EventSymbolId!).ToHashSet(StringComparer.Ordinal);
        foreach (SemanticCompilationUnit unit in context.ProjectionUnits)
        {
            SemanticModel model = context.Compilation.GetSemanticModel(unit.SyntaxTree);
            foreach (AssignmentExpressionSyntax syntax in unit.SyntaxTree.GetRoot().DescendantNodes().OfType<AssignmentExpressionSyntax>())
            {
                if (model.GetOperation(syntax) is not IEventAssignmentOperation assignment) continue;
                if (assignment.EventReference is IEventReferenceOperation eventReference
                    && generatedEvents.Contains(SemanticSymbolProjector.GetSymbolId(eventReference.Event))) continue;
                diagnostics.Add(Error("ASCS5211",
                    "Event += and -= require a selected generated UE event with a validated language contract.",
                    unit.IsPrimary ? SemanticSpanFactory.Create(unit.SourceText, syntax.Span) : SemanticSpanFactory.Empty));
            }
        }
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
                string? parameterDirections = ReadStringArgument(attribute, 2);
                string returnType = ReadStringArgument(attribute, 3)
                    ?? "global::System.Void";
                string[] parameters = parameterTypes is null || parameterTypes.Length == 0
                    ? Array.Empty<string>()
                    : parameterTypes.Split(';', StringSplitOptions.None);
                string[] directions = parameterDirections is null
                    ? Enumerable.Repeat("none", parameters.Length).ToArray()
                    : parameterDirections.Length == 0
                        ? Array.Empty<string>()
                        : parameterDirections.Split(';', StringSplitOptions.None);
                bool valid = subscriptionId is not null
                    && IsStableId(subscriptionId)
                    && field.IsConst
                    && field.Type.SpecialType == SpecialType.System_String
                    && field.ConstantValue is string value
                    && string.Equals(value, subscriptionId, StringComparison.Ordinal)
                    && parameterTypes is not null
                    && parameters.All(parameter => !string.IsNullOrWhiteSpace(parameter)
                        && string.Equals(parameter, parameter.Trim(), StringComparison.Ordinal))
                    && directions.Length == parameters.Length
                    && directions.All(direction => direction is "none" or "ref" or "out")
                    && !string.IsNullOrWhiteSpace(returnType)
                    && string.Equals(returnType, returnType.Trim(), StringComparison.Ordinal);
                if (!valid)
                {
                    diagnostics.Add(Error(
                        "ASCS5201",
                        $"Delegate event contract '{field.Name}' is malformed.",
                        span));
                    continue;
                }

                EventContract contract = new(
                    subscriptionId!,
                    parameters,
                    directions,
                    returnType);
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
        IReadOnlyList<string> expectedTypes,
        IReadOnlyList<string> expectedDirections)
    {
        return parameters.Count == expectedTypes.Count
            && parameters.Count == expectedDirections.Count
            && parameters.Select((parameter, index) => DirectionMatches(
                    parameter.RefKind,
                    expectedDirections[index])
                && TypeMatches(parameter.Type, expectedTypes[index]))
                .All(matches => matches);
    }

    private static bool DirectionMatches(RefKind refKind, string expectedDirection)
    {
        return expectedDirection switch
        {
            "none" => refKind == RefKind.None,
            "ref" => refKind == RefKind.Ref,
            "out" => refKind == RefKind.Out,
            _ => false,
        };
    }

    private static bool TypeMatches(ITypeSymbol type, string expectedType)
    {
        if (string.Equals(
                type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat),
                expectedType,
                StringComparison.Ordinal))
        {
            return true;
        }

        string? specialTypeName = type.SpecialType switch
        {
            SpecialType.System_Void => "global::System.Void",
            SpecialType.System_Boolean => "global::System.Boolean",
            SpecialType.System_Byte => "global::System.Byte",
            SpecialType.System_SByte => "global::System.SByte",
            SpecialType.System_Int16 => "global::System.Int16",
            SpecialType.System_UInt16 => "global::System.UInt16",
            SpecialType.System_Int32 => "global::System.Int32",
            SpecialType.System_UInt32 => "global::System.UInt32",
            SpecialType.System_Int64 => "global::System.Int64",
            SpecialType.System_UInt64 => "global::System.UInt64",
            SpecialType.System_Single => "global::System.Single",
            SpecialType.System_Double => "global::System.Double",
            SpecialType.System_Char => "global::System.Char",
            SpecialType.System_String => "global::System.String",
            _ => null,
        };
        return string.Equals(specialTypeName, expectedType, StringComparison.Ordinal);
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
