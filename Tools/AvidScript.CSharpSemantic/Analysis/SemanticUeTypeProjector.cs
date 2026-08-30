using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace AvidScript.CSharpSemantic;

internal static class SemanticUeTypeProjector
{
    private const string ActorKind = "actor";
    private const string ActorComponentKind = "actor_component";
    private const string WorldSubsystemKind = "world_subsystem";
    private const string GameInstanceSubsystemKind = "game_instance_subsystem";

    public static SemanticUeTypeProjection Project(
        SemanticCompilationContext context,
        SemanticTypeRegistry typeRegistry)
    {
        ArgumentNullException.ThrowIfNull(context);
        ArgumentNullException.ThrowIfNull(typeRegistry);

        UeTypeSymbols symbols = UeTypeSymbols.Create(context.Compilation);
        if (symbols.ClassAttribute is null)
        {
            return new SemanticUeTypeProjection(
                Array.Empty<SemanticUeTypeDeclaration>(),
                Array.Empty<SemanticDiagnostic>());
        }

        SemanticModel semanticModel = context.Compilation.GetSemanticModel(context.PrimaryUnit.SyntaxTree);
        INamedTypeSymbol[] candidates = context.PrimaryUnit.SyntaxTree.GetRoot()
            .DescendantNodes()
            .OfType<ClassDeclarationSyntax>()
            .Select(declaration => semanticModel.GetDeclaredSymbol(declaration))
            .OfType<INamedTypeSymbol>()
            .Where(type => GetAttributes(type, symbols.ClassAttribute).Length > 0)
            .GroupBy(SemanticSymbolProjector.GetSymbolId, StringComparer.Ordinal)
            .Select(group => group.First())
            .OrderBy(SemanticSymbolProjector.GetSymbolId, StringComparer.Ordinal)
            .ToArray();
        HashSet<INamedTypeSymbol> candidateSet = candidates.ToHashSet<INamedTypeSymbol>(
            SymbolEqualityComparer.Default);
        Dictionary<INamedTypeSymbol, string?> resolvedKinds = new(SymbolEqualityComparer.Default);
        List<SemanticDiagnostic> diagnostics = new();
        List<SemanticUeTypeDeclaration> declarations = new();
        HashSet<string> engineNames = new(StringComparer.Ordinal);

        foreach (INamedTypeSymbol type in candidates)
        {
            AttributeData[] classAttributes = GetAttributes(type, symbols.ClassAttribute);
            if (classAttributes.Length > 1)
            {
                diagnostics.Add(CreateAttributeDiagnostic(
                    "ASUE1001",
                    "UClass must not be duplicated on a script type.",
                    context,
                    classAttributes[1]));
            }

            ClassDeclarationSyntax? declaration = type.DeclaringSyntaxReferences
                .Select(reference => reference.GetSyntax())
                .OfType<ClassDeclarationSyntax>()
                .FirstOrDefault(node => node.SyntaxTree == context.PrimaryUnit.SyntaxTree);
            if (declaration is null)
            {
                continue;
            }

            if (type.ContainingType is not null
                || type.IsStatic
                || type.Arity != 0
                || !declaration.Modifiers.Any(modifier => modifier.ValueText == "partial"))
            {
                diagnostics.Add(CreateNodeDiagnostic(
                    "ASUE1002",
                    "UClass script types must be top-level, non-static, non-generic partial classes.",
                    context,
                    declaration));
            }

            string? kind = ResolveKind(
                type,
                candidateSet,
                symbols,
                resolvedKinds,
                new HashSet<INamedTypeSymbol>(SymbolEqualityComparer.Default));
            if (kind is null)
            {
                diagnostics.Add(CreateNodeDiagnostic(
                    "ASUE1003",
                    "UClass script types must derive from an AvidScript UE marker base or another UClass script type.",
                    context,
                    declaration));
                kind = "invalid";
            }

            string engineName = GetEngineName(type, classAttributes[0]);
            if (!IsValidEngineName(engineName) || !engineNames.Add(engineName))
            {
                diagnostics.Add(CreateAttributeDiagnostic(
                    "ASUE1004",
                    "UClass engine names must be unique ASCII UE reflection identifiers.",
                    context,
                    classAttributes[0]));
            }

            declarations.Add(new SemanticUeTypeDeclaration(
                typeRegistry.Register(type),
                SemanticSymbolProjector.GetSymbolId(type),
                engineName,
                kind,
                type.BaseType is null ? "type:none" : typeRegistry.Register(type.BaseType),
                ProjectClassFlags(type, classAttributes[0]),
                ProjectProperties(context, type, symbols, typeRegistry, diagnostics),
                ProjectFunctions(context, type, symbols, diagnostics),
                SemanticSpanFactory.Create(context.SourceText, declaration.Span)));
        }

        return new SemanticUeTypeProjection(
            declarations,
            diagnostics
                .OrderBy(diagnostic => diagnostic.Span.Start)
                .ThenBy(diagnostic => diagnostic.Code, StringComparer.Ordinal)
                .ToArray());
    }

    private static string? ResolveKind(
        INamedTypeSymbol type,
        ISet<INamedTypeSymbol> candidates,
        UeTypeSymbols symbols,
        IDictionary<INamedTypeSymbol, string?> cache,
        ISet<INamedTypeSymbol> visiting)
    {
        if (cache.TryGetValue(type, out string? cached))
        {
            return cached;
        }
        if (!visiting.Add(type))
        {
            return null;
        }

        INamedTypeSymbol? baseType = type.BaseType;
        string? kind = baseType switch
        {
            not null when SymbolEqualityComparer.Default.Equals(baseType, symbols.ActorBase) => ActorKind,
            not null when SymbolEqualityComparer.Default.Equals(baseType, symbols.ActorComponentBase) => ActorComponentKind,
            not null when SymbolEqualityComparer.Default.Equals(baseType, symbols.WorldSubsystemBase) => WorldSubsystemKind,
            not null when SymbolEqualityComparer.Default.Equals(baseType, symbols.GameInstanceSubsystemBase) => GameInstanceSubsystemKind,
            not null when candidates.Contains(baseType) => ResolveKind(baseType, candidates, symbols, cache, visiting),
            _ => null,
        };
        visiting.Remove(type);
        cache[type] = kind;
        return kind;
    }

    private static IReadOnlyList<SemanticUePropertyDeclaration> ProjectProperties(
        SemanticCompilationContext context,
        INamedTypeSymbol owner,
        UeTypeSymbols symbols,
        SemanticTypeRegistry typeRegistry,
        ICollection<SemanticDiagnostic> diagnostics)
    {
        List<SemanticUePropertyDeclaration> properties = new();
        foreach (ISymbol member in owner.GetMembers().OrderBy(
            SemanticSymbolProjector.GetSymbolId,
            StringComparer.Ordinal))
        {
            AttributeData[] attributes = GetAttributes(member, symbols.PropertyAttribute);
            if (attributes.Length == 0)
            {
                continue;
            }
            if (attributes.Length > 1)
            {
                diagnostics.Add(CreateAttributeDiagnostic(
                    "ASUE1101",
                    "UProperty must not be duplicated on a member.",
                    context,
                    attributes[1]));
            }

            ITypeSymbol? valueType = member switch
            {
                IFieldSymbol field when !field.IsImplicitlyDeclared && !field.IsStatic && !field.IsConst => field.Type,
                IPropertySymbol property when !property.IsStatic && !property.IsIndexer => property.Type,
                _ => null,
            };
            if (valueType is null)
            {
                diagnostics.Add(CreateSymbolDiagnostic(
                    "ASUE1102",
                    "UProperty is only valid on instance fields or non-indexed instance properties.",
                    context,
                    member));
                continue;
            }

            AttributeData attribute = attributes[0];
            bool editAnywhere = GetNamedBoolean(attribute, "EditAnywhere");
            bool visibleAnywhere = GetNamedBoolean(attribute, "VisibleAnywhere");
            bool blueprintReadOnly = GetNamedBoolean(attribute, "BlueprintReadOnly");
            bool blueprintReadWrite = GetNamedBoolean(attribute, "BlueprintReadWrite");
            string replicatedUsing = GetNamedString(attribute, "ReplicatedUsing");
            if (editAnywhere && visibleAnywhere || blueprintReadOnly && blueprintReadWrite)
            {
                diagnostics.Add(CreateAttributeDiagnostic(
                    "ASUE1103",
                    "UProperty edit and Blueprint access flags must not conflict.",
                    context,
                    attribute));
            }
            if (!string.IsNullOrEmpty(replicatedUsing) && !HasValidRepNotify(owner, replicatedUsing))
            {
                diagnostics.Add(CreateAttributeDiagnostic(
                    "ASUE1104",
                    "UProperty ReplicatedUsing must name a parameterless void instance method on this type or a base type.",
                    context,
                    attribute));
            }

            List<string> flags = new();
            AddFlag(flags, editAnywhere, "edit_anywhere");
            AddFlag(flags, visibleAnywhere, "visible_anywhere");
            AddFlag(flags, blueprintReadOnly, "blueprint_read_only");
            AddFlag(flags, blueprintReadWrite, "blueprint_read_write");
            AddFlag(flags, GetNamedBoolean(attribute, "Replicated") || replicatedUsing.Length > 0, "replicated");
            AddFlag(flags, GetNamedBoolean(attribute, "SaveGame"), "save_game");
            AddFlag(flags, GetNamedBoolean(attribute, "Transient"), "transient");
            SemanticUePropertyInitializer? initializer = ProjectPropertyInitializer(
                context,
                member,
                valueType,
                diagnostics);
            properties.Add(new SemanticUePropertyDeclaration(
                SemanticSymbolProjector.GetSymbolId(member),
                member.Name,
                typeRegistry.Register(valueType),
                flags,
                GetNamedString(attribute, "Category"),
                replicatedUsing,
                initializer,
                GetSymbolSpan(context, member)));
        }

        return properties;
    }

    private static SemanticUePropertyInitializer? ProjectPropertyInitializer(
        SemanticCompilationContext context,
        ISymbol member,
        ITypeSymbol valueType,
        ICollection<SemanticDiagnostic> diagnostics)
    {
        ExpressionSyntax? expression = member.DeclaringSyntaxReferences
            .Where(reference => reference.SyntaxTree == context.PrimaryUnit.SyntaxTree)
            .Select(reference => reference.GetSyntax())
            .Select(node => node switch
            {
                PropertyDeclarationSyntax property => property.Initializer?.Value,
                VariableDeclaratorSyntax field => field.Initializer?.Value,
                _ => null,
            })
            .FirstOrDefault(node => node is not null);
        if (expression is null)
        {
            return null;
        }

        SemanticModel semanticModel = context.Compilation.GetSemanticModel(expression.SyntaxTree);
        Optional<object?> constant = semanticModel.GetConstantValue(expression);
        if (!constant.HasValue
            || !TryFormatInitializer(valueType, constant.Value, out SemanticUePropertyInitializer? initializer))
        {
            diagnostics.Add(CreateNodeDiagnostic(
                "ASUE1105",
                "UProperty initializers must be deterministic primitive compile-time constants.",
                context,
                expression));
            return null;
        }
        return initializer;
    }

    private static bool TryFormatInitializer(
        ITypeSymbol valueType,
        object? value,
        out SemanticUePropertyInitializer? initializer)
    {
        initializer = null;
        if (value is null)
        {
            return false;
        }

        string kind;
        string canonicalValue;
        try
        {
            (kind, canonicalValue) = valueType.SpecialType switch
            {
                SpecialType.System_Boolean => ("bool", Convert.ToBoolean(value, CultureInfo.InvariantCulture) ? "true" : "false"),
                SpecialType.System_Byte => ("uint8", Convert.ToByte(value, CultureInfo.InvariantCulture).ToString(CultureInfo.InvariantCulture)),
                SpecialType.System_SByte => ("int8", Convert.ToSByte(value, CultureInfo.InvariantCulture).ToString(CultureInfo.InvariantCulture)),
                SpecialType.System_Int16 => ("int16", Convert.ToInt16(value, CultureInfo.InvariantCulture).ToString(CultureInfo.InvariantCulture)),
                SpecialType.System_UInt16 => ("uint16", Convert.ToUInt16(value, CultureInfo.InvariantCulture).ToString(CultureInfo.InvariantCulture)),
                SpecialType.System_Int32 => ("int32", Convert.ToInt32(value, CultureInfo.InvariantCulture).ToString(CultureInfo.InvariantCulture)),
                SpecialType.System_UInt32 => ("uint32", Convert.ToUInt32(value, CultureInfo.InvariantCulture).ToString(CultureInfo.InvariantCulture)),
                SpecialType.System_Int64 => ("int64", Convert.ToInt64(value, CultureInfo.InvariantCulture).ToString(CultureInfo.InvariantCulture)),
                SpecialType.System_UInt64 => ("uint64", Convert.ToUInt64(value, CultureInfo.InvariantCulture).ToString(CultureInfo.InvariantCulture)),
                SpecialType.System_Single => ("float32", FormatFloatingPoint(Convert.ToSingle(value, CultureInfo.InvariantCulture))),
                SpecialType.System_Double => ("float64", FormatFloatingPoint(Convert.ToDouble(value, CultureInfo.InvariantCulture))),
                SpecialType.System_String => ("string", Convert.ToString(value, CultureInfo.InvariantCulture) ?? string.Empty),
                _ => (string.Empty, string.Empty),
            };
        }
        catch (Exception exception) when (exception is FormatException or InvalidCastException or OverflowException)
        {
            return false;
        }
        if (kind.Length == 0)
        {
            return false;
        }
        if ((kind == "float32"
                && (!float.TryParse(canonicalValue, NumberStyles.Float, CultureInfo.InvariantCulture, out float floatValue)
                    || !float.IsFinite(floatValue)))
            || (kind == "float64"
                && (!double.TryParse(canonicalValue, NumberStyles.Float, CultureInfo.InvariantCulture, out double doubleValue)
                    || !double.IsFinite(doubleValue))))
        {
            return false;
        }
        initializer = new SemanticUePropertyInitializer(kind, canonicalValue);
        return true;
    }

    private static string FormatFloatingPoint(float value)
    {
        return value.ToString("R", CultureInfo.InvariantCulture);
    }

    private static string FormatFloatingPoint(double value)
    {
        return value.ToString("R", CultureInfo.InvariantCulture);
    }

    private static IReadOnlyList<SemanticUeFunctionDeclaration> ProjectFunctions(
        SemanticCompilationContext context,
        INamedTypeSymbol owner,
        UeTypeSymbols symbols,
        ICollection<SemanticDiagnostic> diagnostics)
    {
        List<SemanticUeFunctionDeclaration> functions = new();
        foreach (IMethodSymbol method in owner.GetMembers()
            .OfType<IMethodSymbol>()
            .OrderBy(SemanticSymbolProjector.GetSymbolId, StringComparer.Ordinal))
        {
            AttributeData[] attributes = GetAttributes(method, symbols.FunctionAttribute);
            bool isLifecycleOverride = IsLifecycleOverride(method, symbols);
            if (attributes.Length == 0 && !isLifecycleOverride)
            {
                continue;
            }
            if (attributes.Length > 1)
            {
                diagnostics.Add(CreateAttributeDiagnostic(
                    "ASUE1201",
                    "UFunction must not be duplicated on a method.",
                    context,
                    attributes[1]));
            }
            if (method.MethodKind != MethodKind.Ordinary || method.IsStatic || method.Arity != 0)
            {
                diagnostics.Add(CreateSymbolDiagnostic(
                    "ASUE1202",
                    "UFunction is only valid on non-generic instance methods.",
                    context,
                    method));
                continue;
            }

            AttributeData? attribute = attributes.FirstOrDefault();
            bool blueprintPure = GetNamedBoolean(attribute, "BlueprintPure");
            bool blueprintNativeEvent = GetNamedBoolean(attribute, "BlueprintNativeEvent");
            bool blueprintImplementableEvent = GetNamedBoolean(attribute, "BlueprintImplementableEvent");
            bool server = GetNamedBoolean(attribute, "Server");
            bool client = GetNamedBoolean(attribute, "Client");
            bool multicast = GetNamedBoolean(attribute, "NetMulticast");
            bool reliable = GetNamedBoolean(attribute, "Reliable");
            bool unreliable = GetNamedBoolean(attribute, "Unreliable");
            int networkDirections = (server ? 1 : 0) + (client ? 1 : 0) + (multicast ? 1 : 0);
            if (blueprintNativeEvent && blueprintImplementableEvent
                || networkDirections > 1
                || reliable && unreliable
                || (reliable || unreliable) && networkDirections == 0
                || blueprintPure && networkDirections != 0)
            {
                diagnostics.Add(CreateAttributeDiagnostic(
                    "ASUE1203",
                    "UFunction Blueprint and network flags form an invalid combination.",
                    context,
                    attribute!));
            }

            List<string> flags = new();
            AddFlag(flags, GetNamedBoolean(attribute, "BlueprintCallable") || blueprintPure, "blueprint_callable");
            AddFlag(flags, blueprintPure, "blueprint_pure");
            AddFlag(flags, blueprintNativeEvent, "blueprint_native_event");
            AddFlag(flags, blueprintImplementableEvent, "blueprint_implementable_event");
            AddFlag(flags, server, "server");
            AddFlag(flags, client, "client");
            AddFlag(flags, multicast, "net_multicast");
            AddFlag(flags, reliable, "reliable");
            AddFlag(flags, unreliable, "unreliable");
            AddFlag(flags, isLifecycleOverride, "lifecycle");
            AddFlag(flags, method.IsOverride, "override");
            functions.Add(new SemanticUeFunctionDeclaration(
                SemanticSymbolProjector.GetSymbolId(method),
                method.Name,
                flags,
                GetNamedString(attribute, "Category"),
                GetSymbolSpan(context, method)));
        }

        return functions;
    }

    private static bool IsLifecycleOverride(IMethodSymbol method, UeTypeSymbols symbols)
    {
        if (!method.IsOverride || method.OverriddenMethod is null)
        {
            return false;
        }
        IMethodSymbol root = method.OverriddenMethod;
        while (root.OverriddenMethod is not null)
        {
            root = root.OverriddenMethod;
        }
        INamedTypeSymbol owner = root.ContainingType;
        return SymbolEqualityComparer.Default.Equals(owner, symbols.ActorBase)
            || SymbolEqualityComparer.Default.Equals(owner, symbols.ActorComponentBase)
            || SymbolEqualityComparer.Default.Equals(owner, symbols.WorldSubsystemBase)
            || SymbolEqualityComparer.Default.Equals(owner, symbols.GameInstanceSubsystemBase);
    }

    private static bool HasValidRepNotify(INamedTypeSymbol owner, string methodName)
    {
        for (INamedTypeSymbol? cursor = owner; cursor is not null; cursor = cursor.BaseType)
        {
            if (cursor.GetMembers(methodName).OfType<IMethodSymbol>().Any(method =>
                !method.IsStatic
                && method.MethodKind == MethodKind.Ordinary
                && method.Parameters.Length == 0
                && method.ReturnsVoid))
            {
                return true;
            }
        }
        return false;
    }

    private static IReadOnlyList<string> ProjectClassFlags(INamedTypeSymbol type, AttributeData attribute)
    {
        List<string> flags = new();
        AddFlag(flags, GetNamedBoolean(attribute, "Blueprintable", true), "blueprintable");
        AddFlag(flags, GetNamedBoolean(attribute, "BlueprintType", true), "blueprint_type");
        AddFlag(flags, type.IsAbstract || GetNamedBoolean(attribute, "Abstract"), "abstract");
        AddFlag(flags, GetNamedBoolean(attribute, "NotPlaceable"), "not_placeable");
        AddFlag(flags, GetNamedBoolean(attribute, "Transient"), "transient");
        return flags;
    }

    private static string GetEngineName(INamedTypeSymbol type, AttributeData attribute)
    {
        string requested = GetNamedString(attribute, "Name");
        return requested.Length == 0 ? type.Name : requested;
    }

    private static bool IsValidEngineName(string value)
    {
        if (value.Length == 0 || !IsAsciiIdentifierStart(value[0]))
        {
            return false;
        }
        return value.Skip(1).All(IsAsciiIdentifierPart);
    }

    private static bool IsAsciiIdentifierStart(char value)
    {
        return value is >= 'A' and <= 'Z' or >= 'a' and <= 'z' or '_';
    }

    private static bool IsAsciiIdentifierPart(char value)
    {
        return IsAsciiIdentifierStart(value) || value is >= '0' and <= '9';
    }

    private static AttributeData[] GetAttributes(ISymbol symbol, INamedTypeSymbol? canonicalAttribute)
    {
        if (canonicalAttribute is null)
        {
            return Array.Empty<AttributeData>();
        }
        return symbol.GetAttributes()
            .Where(attribute => SymbolEqualityComparer.Default.Equals(attribute.AttributeClass, canonicalAttribute))
            .ToArray();
    }

    private static bool GetNamedBoolean(AttributeData? attribute, string name, bool defaultValue = false)
    {
        if (attribute is null)
        {
            return defaultValue;
        }
        foreach (KeyValuePair<string, TypedConstant> argument in attribute.NamedArguments)
        {
            if (argument.Key == name && argument.Value.Value is bool value)
            {
                return value;
            }
        }
        return defaultValue;
    }

    private static string GetNamedString(AttributeData? attribute, string name)
    {
        if (attribute is null)
        {
            return string.Empty;
        }
        foreach (KeyValuePair<string, TypedConstant> argument in attribute.NamedArguments)
        {
            if (argument.Key == name && argument.Value.Value is string value)
            {
                return value;
            }
        }
        return string.Empty;
    }

    private static void AddFlag(ICollection<string> flags, bool condition, string flag)
    {
        if (condition)
        {
            flags.Add(flag);
        }
    }

    private static SemanticSpan GetSymbolSpan(SemanticCompilationContext context, ISymbol symbol)
    {
        SyntaxReference? reference = symbol.DeclaringSyntaxReferences.FirstOrDefault(item =>
            item.SyntaxTree == context.PrimaryUnit.SyntaxTree);
        return reference is null
            ? SemanticSpanFactory.Empty
            : SemanticSpanFactory.Create(context.SourceText, reference.Span);
    }

    private static SemanticDiagnostic CreateAttributeDiagnostic(
        string code,
        string message,
        SemanticCompilationContext context,
        AttributeData attribute)
    {
        SyntaxReference? reference = attribute.ApplicationSyntaxReference;
        return new SemanticDiagnostic(
            code,
            "error",
            message,
            reference is null
                ? SemanticSpanFactory.Empty
                : SemanticSpanFactory.Create(context.SourceText, reference.Span));
    }

    private static SemanticDiagnostic CreateNodeDiagnostic(
        string code,
        string message,
        SemanticCompilationContext context,
        SyntaxNode node)
    {
        return new SemanticDiagnostic(
            code,
            "error",
            message,
            SemanticSpanFactory.Create(context.SourceText, node.Span));
    }

    private static SemanticDiagnostic CreateSymbolDiagnostic(
        string code,
        string message,
        SemanticCompilationContext context,
        ISymbol symbol)
    {
        return new SemanticDiagnostic(code, "error", message, GetSymbolSpan(context, symbol));
    }

    private sealed record UeTypeSymbols(
        INamedTypeSymbol? ClassAttribute,
        INamedTypeSymbol? PropertyAttribute,
        INamedTypeSymbol? FunctionAttribute,
        INamedTypeSymbol? ActorBase,
        INamedTypeSymbol? ActorComponentBase,
        INamedTypeSymbol? WorldSubsystemBase,
        INamedTypeSymbol? GameInstanceSubsystemBase)
    {
        public static UeTypeSymbols Create(Compilation compilation)
        {
            return new UeTypeSymbols(
                compilation.GetTypeByMetadataName("AvidScript.UClassAttribute"),
                compilation.GetTypeByMetadataName("AvidScript.UPropertyAttribute"),
                compilation.GetTypeByMetadataName("AvidScript.UFunctionAttribute"),
                compilation.GetTypeByMetadataName("AvidScript.AvidActor"),
                compilation.GetTypeByMetadataName("AvidScript.AvidActorComponent"),
                compilation.GetTypeByMetadataName("AvidScript.AvidWorldSubsystem"),
                compilation.GetTypeByMetadataName("AvidScript.AvidGameInstanceSubsystem"));
        }
    }
}
