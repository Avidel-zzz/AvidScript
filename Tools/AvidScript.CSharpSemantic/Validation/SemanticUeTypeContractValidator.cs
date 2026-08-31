using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;

namespace AvidScript.CSharpSemantic;

public static class SemanticUeTypeContractValidator
{
    private static readonly IReadOnlySet<string> TypeKinds = CreateSet(
        "actor",
        "actor_component",
        "world_subsystem",
        "game_instance_subsystem");

    private static readonly IReadOnlyDictionary<string, string> MarkerBaseTypes =
        new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["actor"] = "global::AvidScript.AvidActor",
            ["actor_component"] = "global::AvidScript.AvidActorComponent",
            ["world_subsystem"] = "global::AvidScript.AvidWorldSubsystem",
            ["game_instance_subsystem"] = "global::AvidScript.AvidGameInstanceSubsystem",
        };

    private static readonly IReadOnlySet<string> ClassFlags = CreateSet(
        "blueprintable",
        "blueprint_type",
        "abstract",
        "not_placeable",
        "transient");

    private static readonly IReadOnlySet<string> PropertyFlags = CreateSet(
        "edit_anywhere",
        "visible_anywhere",
        "blueprint_read_only",
        "blueprint_read_write",
        "replicated",
        "save_game",
        "transient");

    private static readonly IReadOnlySet<string> FunctionFlags = CreateSet(
        "blueprint_callable",
        "blueprint_pure",
        "blueprint_native_event",
        "blueprint_implementable_event",
        "server",
        "client",
        "net_multicast",
        "reliable",
        "unreliable",
        "lifecycle",
        "override");

    public static bool TryValidate(SemanticDocument document, out string error)
    {
        ArgumentNullException.ThrowIfNull(document);
        error = string.Empty;
        if (document.UeTypeDeclarations is null)
        {
            return Fail("ue_type_declarations is missing.", out error);
        }
        if (document.SchemaVersion < 19)
        {
            return document.UeTypeDeclarations.Count == 0
                || Fail("Schema versions before 19 cannot contain UE type declarations.", out error);
        }
        if (!TryIndex(document.Types, type => type.Id, "type", out Dictionary<string, SemanticType> types, out error)
            || !TryIndex(document.Symbols, symbol => symbol.Id, "symbol", out Dictionary<string, SemanticSymbol> symbols, out error)
            || !TryIndex(document.Callables, callable => callable.MethodSymbolId, "callable", out Dictionary<string, SemanticCallable> callables, out error))
        {
            return false;
        }

        IReadOnlyList<SemanticUeTypeDeclaration> declarations = document.UeTypeDeclarations;
        if (declarations.Any(declaration => declaration is null)
            || !Unique(declarations.Select(declaration => declaration.TypeId))
            || !Unique(declarations.Select(declaration => declaration.SymbolId))
            || !UniqueUeNames(declarations.Select(declaration => declaration.EngineName))
            || !IsOrdinalSorted(declarations.Select(declaration => declaration.SymbolId)))
        {
            return Fail("UE type identities must be non-null, unique and sorted by symbol id.", out error);
        }

        foreach (SemanticUeTypeDeclaration declaration in declarations)
        {
            if (!ValidateDeclaration(declaration, types, symbols, callables, out error))
            {
                return false;
            }
        }
        return ValidateInheritance(declarations, types, out error);
    }

    private static bool ValidateDeclaration(
        SemanticUeTypeDeclaration declaration,
        IReadOnlyDictionary<string, SemanticType> types,
        IReadOnlyDictionary<string, SemanticSymbol> symbols,
        IReadOnlyDictionary<string, SemanticCallable> callables,
        out string error)
    {
        error = string.Empty;
        if (!types.ContainsKey(declaration.TypeId)
            || !types.ContainsKey(declaration.BaseTypeId)
            || !TypeKinds.Contains(declaration.Kind)
            || !IsIdentifier(declaration.EngineName)
            || declaration.Flags is null
            || declaration.Properties is null
            || declaration.Functions is null
            || declaration.Span is null
            || !ValidateFlags(declaration.Flags, ClassFlags)
            || !symbols.TryGetValue(declaration.SymbolId, out SemanticSymbol? typeSymbol)
            || typeSymbol.Kind != "type"
            || typeSymbol.TypeId != declaration.TypeId)
        {
            return Fail($"UE type declaration '{declaration.SymbolId}' is malformed.", out error);
        }
        if (!ValidateProperties(declaration, types, symbols, out error))
        {
            return false;
        }
        string[] reflectedMemberNames = declaration.Properties.Select(property => property.Name)
            .Concat(declaration.Functions.Select(function => function.Name))
            .ToArray();
        if (!UniqueUeNames(reflectedMemberNames))
        {
            return Fail(
                $"UE members on '{declaration.SymbolId}' must have case-insensitively unique reflection names.",
                out error);
        }
        if (!ValidateFunctions(
            declaration,
            symbols,
            callables,
            reflectedMemberNames.ToHashSet(StringComparer.OrdinalIgnoreCase),
            out error))
        {
            return false;
        }
        if (declaration.Kind is "world_subsystem" or "game_instance_subsystem"
            && (declaration.Properties.Any(property => property.Flags.Contains("replicated"))
                || declaration.Functions.Any(function => function.Flags.Any(flag =>
                    flag is "server" or "client" or "net_multicast"))))
        {
            return Fail(
                $"UE subsystem '{declaration.SymbolId}' cannot own replicated properties or RPC functions.",
                out error);
        }
        return true;
    }

    private static bool ValidateProperties(
        SemanticUeTypeDeclaration declaration,
        IReadOnlyDictionary<string, SemanticType> types,
        IReadOnlyDictionary<string, SemanticSymbol> symbols,
        out string error)
    {
        error = string.Empty;
        if (declaration.Properties.Any(property => property is null)
            || !Unique(declaration.Properties.Select(property => property.SymbolId))
            || !Unique(declaration.Properties.Select(property => property.Name))
            || !IsOrdinalSorted(declaration.Properties.Select(property => property.SymbolId)))
        {
            return Fail($"UE properties on '{declaration.SymbolId}' must be unique and sorted.", out error);
        }
        foreach (SemanticUePropertyDeclaration property in declaration.Properties)
        {
            if (!IsIdentifier(property.Name)
                || !types.ContainsKey(property.TypeId)
                || property.Flags is null
                || property.Category is null
                || property.ReplicatedUsing is null
                || property.Span is null
                || !ValidateFlags(property.Flags, PropertyFlags)
                || property.Flags.Contains("edit_anywhere") && property.Flags.Contains("visible_anywhere")
                || property.Flags.Contains("blueprint_read_only") && property.Flags.Contains("blueprint_read_write")
                || property.ReplicatedUsing.Length > 0 && !property.Flags.Contains("replicated")
                || property.ReplicatedUsing.Length > 0 && !IsIdentifier(property.ReplicatedUsing)
                || !ValidateInitializer(property.Initializer, types[property.TypeId])
                || !symbols.TryGetValue(property.SymbolId, out SemanticSymbol? symbol)
                || symbol.Kind is not ("field" or "property")
                || symbol.Name != property.Name
                || symbol.TypeId != property.TypeId
                || symbol.ContainingSymbolId != declaration.SymbolId)
            {
                return Fail($"UE property '{property.SymbolId}' is malformed.", out error);
            }
        }
        return true;
    }

    private static bool ValidateInitializer(
        SemanticUePropertyInitializer? initializer,
        SemanticType propertyType)
    {
        if (initializer is null)
        {
            return true;
        }
        if (initializer.Kind != propertyType.CanonicalName || initializer.CanonicalValue is null)
        {
            return false;
        }
        string value = initializer.CanonicalValue;
        return initializer.Kind switch
        {
            "bool" => value is "true" or "false",
            "uint8" => byte.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out _),
            "int8" => sbyte.TryParse(value, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out _),
            "int16" => short.TryParse(value, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out _),
            "uint16" => ushort.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out _),
            "int32" => int.TryParse(value, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out _),
            "uint32" => uint.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out _),
            "int64" => long.TryParse(value, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out _),
            "uint64" => ulong.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out _),
            "float32" => float.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out float parsedFloat)
                && float.IsFinite(parsedFloat),
            "float64" => double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out double parsedDouble)
                && double.IsFinite(parsedDouble),
            "string" => true,
            _ => false,
        };
    }

    private static bool ValidateFunctions(
        SemanticUeTypeDeclaration declaration,
        IReadOnlyDictionary<string, SemanticSymbol> symbols,
        IReadOnlyDictionary<string, SemanticCallable> callables,
        IReadOnlySet<string> reflectedMemberNames,
        out string error)
    {
        error = string.Empty;
        if (declaration.Functions.Any(function => function is null)
            || !Unique(declaration.Functions.Select(function => function.MethodSymbolId))
            || !Unique(declaration.Functions.Select(function => function.Name))
            || !IsOrdinalSorted(declaration.Functions.Select(function => function.MethodSymbolId)))
        {
            return Fail($"UE functions on '{declaration.SymbolId}' must be unique and sorted.", out error);
        }
        foreach (SemanticUeFunctionDeclaration function in declaration.Functions)
        {
            if (!IsIdentifier(function.Name)
                || function.Flags is null
                || function.Category is null
                || function.Span is null
                || !ValidateFlags(function.Flags, FunctionFlags)
                || !ValidateFunctionFlags(function.Flags)
                || !symbols.TryGetValue(function.MethodSymbolId, out SemanticSymbol? symbol)
                || symbol.Kind != "method"
                || symbol.Name != function.Name
                || symbol.IsStatic
                || symbol.ContainingSymbolId != declaration.SymbolId
                || !callables.TryGetValue(function.MethodSymbolId, out SemanticCallable? callable)
                || callable.IsStatic
                || callable.ContainingTypeId != declaration.TypeId
                || callable.Parameters is null
                || !UniqueUeNames(callable.Parameters.Select(parameter => parameter.Name))
                || callable.Parameters.Any(parameter => reflectedMemberNames.Contains(parameter.Name)))
            {
                return Fail($"UE function '{function.MethodSymbolId}' is malformed.", out error);
            }
        }
        return true;
    }

    private static bool ValidateInheritance(
        IReadOnlyList<SemanticUeTypeDeclaration> declarations,
        IReadOnlyDictionary<string, SemanticType> types,
        out string error)
    {
        Dictionary<string, SemanticUeTypeDeclaration> declarationsByTypeId = declarations.ToDictionary(
            declaration => declaration.TypeId,
            StringComparer.Ordinal);
        foreach (SemanticUeTypeDeclaration declaration in declarations)
        {
            HashSet<string> visiting = new(StringComparer.Ordinal) { declaration.TypeId };
            SemanticUeTypeDeclaration cursor = declaration;
            while (declarationsByTypeId.TryGetValue(cursor.BaseTypeId, out SemanticUeTypeDeclaration? parent))
            {
                if (parent.Kind != declaration.Kind || !visiting.Add(parent.TypeId))
                {
                    return Fail($"UE type inheritance for '{declaration.SymbolId}' is cyclic or changes kind.", out error);
                }
                cursor = parent;
            }

            string expectedMarker = MarkerBaseTypes[declaration.Kind];
            if (!types.TryGetValue(cursor.BaseTypeId, out SemanticType? root)
                || root.CanonicalName != expectedMarker)
            {
                return Fail($"UE type inheritance for '{declaration.SymbolId}' does not end at '{expectedMarker}'.", out error);
            }
        }
        error = string.Empty;
        return true;
    }

    private static bool ValidateFunctionFlags(IReadOnlyList<string> flags)
    {
        int networkDirections = Count(flags, "server", "client", "net_multicast");
        return !(flags.Contains("blueprint_native_event") && flags.Contains("blueprint_implementable_event"))
            && networkDirections <= 1
            && !(flags.Contains("reliable") && flags.Contains("unreliable"))
            && (!(flags.Contains("reliable") || flags.Contains("unreliable")) || networkDirections == 1)
            && !(flags.Contains("blueprint_pure") && networkDirections != 0)
            && (!flags.Contains("blueprint_pure") || flags.Contains("blueprint_callable"))
            && (!flags.Contains("lifecycle") || flags.Contains("override"));
    }

    private static int Count(IReadOnlyList<string> values, params string[] candidates)
    {
        return candidates.Count(values.Contains);
    }

    private static bool ValidateFlags(IReadOnlyList<string> flags, IReadOnlySet<string> allowed)
    {
        return flags.All(flag => !string.IsNullOrWhiteSpace(flag) && allowed.Contains(flag))
            && Unique(flags);
    }

    private static bool IsIdentifier(string value)
    {
        return value.Length > 0
            && IsIdentifierStart(value[0])
            && value.Skip(1).All(IsIdentifierPart);
    }

    private static bool IsIdentifierStart(char value)
    {
        return value is >= 'A' and <= 'Z' or >= 'a' and <= 'z' or '_';
    }

    private static bool IsIdentifierPart(char value)
    {
        return IsIdentifierStart(value) || value is >= '0' and <= '9';
    }

    private static bool Unique(IEnumerable<string> values)
    {
        HashSet<string> seen = new(StringComparer.Ordinal);
        return values.All(value => !string.IsNullOrWhiteSpace(value) && seen.Add(value));
    }

    private static bool UniqueUeNames(IEnumerable<string> values)
    {
        HashSet<string> seen = new(StringComparer.OrdinalIgnoreCase);
        return values.All(value => !string.IsNullOrWhiteSpace(value) && seen.Add(value));
    }

    private static bool IsOrdinalSorted(IEnumerable<string> values)
    {
        string[] materialized = values.ToArray();
        return materialized.SequenceEqual(materialized.OrderBy(value => value, StringComparer.Ordinal));
    }

    private static bool TryIndex<T>(
        IReadOnlyList<T>? values,
        Func<T, string> keySelector,
        string kind,
        out Dictionary<string, T> index,
        out string error)
        where T : class
    {
        index = new Dictionary<string, T>(StringComparer.Ordinal);
        error = string.Empty;
        if (values is null)
        {
            return Fail($"Semantic {kind} table is missing.", out error);
        }
        foreach (T? value in values)
        {
            if (value is null)
            {
                return Fail($"Semantic {kind} table contains null.", out error);
            }
            string key = keySelector(value);
            if (string.IsNullOrWhiteSpace(key) || !index.TryAdd(key, value))
            {
                return Fail($"Semantic {kind} identities must be non-empty and unique.", out error);
            }
        }
        return true;
    }

    private static bool Fail(string message, out string error)
    {
        error = message;
        return false;
    }

    private static IReadOnlySet<string> CreateSet(params string[] values)
    {
        return new HashSet<string>(values, StringComparer.Ordinal);
    }
}
