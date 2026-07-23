using System;
using System.Linq;
using AvidScript.CSharpSemantic;

namespace AvidScript.CSharpGuest;

internal static class CSharpClassReferencePolicy
{
    public const string CanonicalPrefix = "global::AvidScript.TSubclassOf";
    public const string CanonicalName = "global::AvidScript.TSubclassOfAActor";
    public const string OrdinalFieldName = "Ordinal";

    public static bool IsType(SemanticType type)
    {
        return type.Kind == "struct"
            && type.IsValueType
            && IsCanonicalName(type.CanonicalName);
    }

    public static bool HasCanonicalField(
        SemanticType type,
        System.Collections.Generic.IReadOnlyList<SemanticSymbol> symbols)
    {
        SemanticSymbol[] fields = symbols
            .Where(symbol => symbol.Kind == "field"
                && !symbol.IsStatic
                && string.Equals(
                    symbol.ContainingSymbolId,
                    "symbol:type:" + type.CanonicalName,
                    StringComparison.Ordinal))
            .ToArray();
        return fields.Length == 1
            && string.Equals(fields[0].Name, OrdinalFieldName, StringComparison.Ordinal)
            && string.Equals(fields[0].TypeId, "type:int32", StringComparison.Ordinal)
            && string.Equals(fields[0].Accessibility, "private", StringComparison.Ordinal)
            && fields[0].IsReadonly;
    }

    public static bool IsIntrinsicConstructor(SemanticCallable callable)
    {
        return callable.IsConstructor
            && !callable.IsStatic
            && IsClassReferenceTypeId(callable.ContainingTypeId)
            && string.Equals(callable.ReturnTypeId, "type:void", StringComparison.Ordinal)
            && callable.Parameters.Count == 1
            && string.Equals(callable.Parameters[0].TypeId, "type:int32", StringComparison.Ordinal)
            && string.Equals(callable.Parameters[0].RefKind, "none", StringComparison.Ordinal);
    }

    public static bool IsIntrinsicConstructor(SemanticDocument document, SemanticCallable callable)
    {
        if (!IsIntrinsicConstructor(callable))
        {
            return false;
        }

        SemanticSymbol? symbol = document.Symbols.SingleOrDefault(item =>
            string.Equals(item.Id, callable.MethodSymbolId, StringComparison.Ordinal));
        return symbol is not null
            && string.Equals(symbol.Accessibility, "internal", StringComparison.Ordinal);
    }

    public static bool IsOrdinalField(SemanticDocument document, string? symbolId)
    {
        SemanticSymbol? field = document.Symbols.SingleOrDefault(symbol =>
            string.Equals(symbol.Id, symbolId, StringComparison.Ordinal));
        return field is not null
            && field.Kind == "field"
            && !field.IsStatic
            && document.Types.SingleOrDefault(type => string.Equals(
                field.ContainingSymbolId,
                "symbol:type:" + type.CanonicalName,
                StringComparison.Ordinal)) is { } type
            && IsType(type)
            && HasCanonicalField(type, document.Symbols);
    }

    public static bool IsClassReferenceTypeId(string? typeId)
    {
        return typeId is not null
            && typeId.StartsWith("type:", StringComparison.Ordinal)
            && IsCanonicalName(typeId["type:".Length..]);
    }

    private static bool IsCanonicalName(string canonicalName)
    {
        if (!canonicalName.StartsWith(CanonicalPrefix, StringComparison.Ordinal))
        {
            return false;
        }

        ReadOnlySpan<char> generatedTypeName = canonicalName.AsSpan(CanonicalPrefix.Length);
        return generatedTypeName.Length != 0
            && IsIdentifierStart(generatedTypeName[0])
            && generatedTypeName[1..].ToString().All(IsIdentifierPart);
    }

    private static bool IsIdentifierStart(char value)
    {
        return value == '_' || char.IsLetter(value);
    }

    private static bool IsIdentifierPart(char value)
    {
        return value == '_' || char.IsLetterOrDigit(value);
    }
}
