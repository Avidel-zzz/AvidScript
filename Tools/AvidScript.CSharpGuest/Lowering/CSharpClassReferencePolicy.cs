using System;
using System.Linq;
using AvidScript.CSharpSemantic;

namespace AvidScript.CSharpGuest;

internal static class CSharpClassReferencePolicy
{
    public const string CanonicalName = "global::AvidScript.TSubclassOfAActor";
    public const string TypeId = "type:" + CanonicalName;
    public const string OrdinalFieldName = "Ordinal";

    public static bool IsType(SemanticType type)
    {
        return type.Kind == "struct"
            && type.IsValueType
            && string.Equals(type.CanonicalName, CanonicalName, StringComparison.Ordinal);
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
            && string.Equals(callable.ContainingTypeId, TypeId, StringComparison.Ordinal)
            && string.Equals(callable.ReturnTypeId, "type:void", StringComparison.Ordinal)
            && callable.Parameters.Count == 1
            && string.Equals(callable.Parameters[0].TypeId, "type:int32", StringComparison.Ordinal)
            && string.Equals(callable.Parameters[0].RefKind, "none", StringComparison.Ordinal);
    }

    public static bool IsOrdinalField(SemanticDocument document, string? symbolId)
    {
        SemanticSymbol? field = document.Symbols.SingleOrDefault(symbol =>
            string.Equals(symbol.Id, symbolId, StringComparison.Ordinal));
        return field is not null
            && field.Kind == "field"
            && !field.IsStatic
            && string.Equals(field.Name, OrdinalFieldName, StringComparison.Ordinal)
            && string.Equals(field.TypeId, "type:int32", StringComparison.Ordinal)
            && string.Equals(
                field.ContainingSymbolId,
                "symbol:type:" + CanonicalName,
                StringComparison.Ordinal);
    }
}
