using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;

namespace AvidScript.CSharpGuest;

internal static class CSharpCompositeValueCapabilityPolicy
{
    public const string TextCanonicalName = "global::AvidScript.FAvidText";
    public const string SoftObjectCanonicalPrefix = "global::AvidScript.FAvidSoftObject<";
    public const string WeakObjectCanonicalPrefix = "global::AvidScript.FAvidWeakObject<";
    public const string ArrayCanonicalPrefix = "global::AvidScript.FAvidArray<";
    public const string SetCanonicalPrefix = "global::AvidScript.FAvidSet<";
    public const string MapCanonicalPrefix = "global::AvidScript.FAvidMap<";
    public const string TokenFieldName = "Token";

    public static bool IsType(SemanticType type)
    {
        return type.Kind == "struct"
            && type.IsValueType
            && IsCanonicalName(type.CanonicalName);
    }

    public static bool HasCanonicalField(
        SemanticType type,
        IReadOnlyList<SemanticSymbol> symbols)
    {
        string definitionCanonicalName = GetDefinitionCanonicalName(type.CanonicalName);
        SemanticSymbol? declaration = symbols.SingleOrDefault(symbol =>
            string.Equals(
                symbol.Id,
                "symbol:type:" + definitionCanonicalName,
                StringComparison.Ordinal));
        SemanticSymbol[] fields = symbols
            .Where(symbol => symbol.Kind == "field"
                && !symbol.IsStatic
                && symbol.IsExecutableReferenceSource
                && string.Equals(
                    symbol.ContainingSymbolId,
                    "symbol:type:" + definitionCanonicalName,
                    StringComparison.Ordinal))
            .ToArray();
        return declaration is { IsExecutableReferenceSource: true }
            && fields.Length == 1
            && string.Equals(fields[0].Name, TokenFieldName, StringComparison.Ordinal)
            && string.Equals(fields[0].TypeId, "type:int32", StringComparison.Ordinal)
            && string.Equals(fields[0].Accessibility, "private", StringComparison.Ordinal)
            && fields[0].IsReadonly;
    }

    public static bool IsIntrinsicConstructor(SemanticCallable callable)
    {
        return callable.IsConstructor
            && !callable.IsStatic
            && callable.HasBody
            && callable.Import is null
            && IsCanonicalName(GetCanonicalName(callable.ContainingTypeId))
            && string.Equals(callable.ReturnTypeId, "type:void", StringComparison.Ordinal)
            && callable.Parameters.Count == 1
            && string.Equals(callable.Parameters[0].TypeId, "type:int32", StringComparison.Ordinal)
            && string.Equals(callable.Parameters[0].RefKind, "none", StringComparison.Ordinal);
    }

    public static bool HasIntrinsicConstructor(
        SemanticType type,
        IReadOnlyList<SemanticSymbol> symbols,
        IReadOnlyList<SemanticCallable> callables)
    {
        string definitionTypeId = "type:" + GetDefinitionCanonicalName(type.CanonicalName);
        return callables.Count(callable =>
                string.Equals(callable.ContainingTypeId, definitionTypeId, StringComparison.Ordinal)
                && IsIntrinsicConstructor(callable)
                && HasInternalConstructorSymbol(symbols, callable)) == 1;
    }

    public static bool IsTokenField(
        SemanticDocument document,
        string? symbolId,
        string? receiverTypeId)
    {
        SemanticSymbol? field = document.Symbols.SingleOrDefault(symbol =>
            string.Equals(symbol.Id, symbolId, StringComparison.Ordinal));
        SemanticType? type = document.Types.SingleOrDefault(item =>
            string.Equals(item.Id, receiverTypeId, StringComparison.Ordinal));
        string definitionCanonicalName = type is null
            ? string.Empty
            : GetDefinitionCanonicalName(type.CanonicalName);
        return field is not null
            && field.Kind == "field"
            && !field.IsStatic
            && field.IsExecutableReferenceSource
            && string.Equals(
                field.ContainingSymbolId,
                "symbol:type:" + definitionCanonicalName,
                StringComparison.Ordinal)
            && type is not null
            && IsType(type)
            && HasCanonicalField(type, document.Symbols);
    }

    private static bool HasInternalConstructorSymbol(
        IReadOnlyList<SemanticSymbol> symbols,
        SemanticCallable callable)
    {
        SemanticSymbol? symbol = symbols.SingleOrDefault(item =>
            string.Equals(item.Id, callable.MethodSymbolId, StringComparison.Ordinal));
        return symbol is not null
            && symbol.Kind == "constructor"
            && !symbol.IsStatic
            && symbol.IsExecutableReferenceSource
            && string.Equals(
                symbol.ContainingSymbolId,
                "symbol:" + callable.ContainingTypeId,
                StringComparison.Ordinal)
            && string.Equals(symbol.Accessibility, "internal", StringComparison.Ordinal);
    }

    private static bool IsCanonicalName(string canonicalName)
    {
        return string.Equals(canonicalName, TextCanonicalName, StringComparison.Ordinal)
            || IsClosedGeneric(canonicalName, SoftObjectCanonicalPrefix)
            || IsClosedGeneric(canonicalName, WeakObjectCanonicalPrefix)
            || IsClosedGeneric(canonicalName, ArrayCanonicalPrefix)
            || IsClosedGeneric(canonicalName, SetCanonicalPrefix)
            || IsClosedGeneric(canonicalName, MapCanonicalPrefix);
    }

    private static bool IsClosedGeneric(string canonicalName, string prefix)
    {
        return canonicalName.StartsWith(prefix, StringComparison.Ordinal)
            && canonicalName.EndsWith(">", StringComparison.Ordinal)
            && canonicalName.Length > prefix.Length + 1;
    }

    private static string GetCanonicalName(string typeId)
    {
        const string Prefix = "type:";
        return typeId.StartsWith(Prefix, StringComparison.Ordinal)
            ? typeId[Prefix.Length..]
            : string.Empty;
    }

    private static string GetDefinitionCanonicalName(string canonicalName)
    {
        if (string.Equals(canonicalName, TextCanonicalName, StringComparison.Ordinal))
        {
            return canonicalName;
        }
        if (IsClosedGeneric(canonicalName, SoftObjectCanonicalPrefix))
        {
            return SoftObjectCanonicalPrefix + "T>";
        }
        if (IsClosedGeneric(canonicalName, WeakObjectCanonicalPrefix))
        {
            return WeakObjectCanonicalPrefix + "T>";
        }
        if (IsClosedGeneric(canonicalName, ArrayCanonicalPrefix))
        {
            return ArrayCanonicalPrefix + "T>";
        }
        if (IsClosedGeneric(canonicalName, SetCanonicalPrefix))
        {
            return SetCanonicalPrefix + "T>";
        }
        if (IsClosedGeneric(canonicalName, MapCanonicalPrefix))
        {
            return MapCanonicalPrefix + "TKey, TValue>";
        }
        return string.Empty;
    }
}
