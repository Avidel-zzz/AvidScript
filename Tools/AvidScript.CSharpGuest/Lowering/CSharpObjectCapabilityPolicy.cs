using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using AvidScript.CSharpSemantic;

namespace AvidScript.CSharpGuest;

internal enum CSharpObjectCapabilityKind
{
    Factory,
    ObjectType,
}

internal static class CSharpObjectCapabilityPolicy
{
    public const string OrdinalFieldName = "Ordinal";
    private const string FactoryPrefix = "global::AvidScript.TObjectFactoryOf";
    private const string ObjectTypePrefix = "global::AvidScript.TObjectTypeOf";
    private const string ProjectFactoriesSymbolId =
        "symbol:type:global::AvidScript.ProjectFactories";
    private const string ProjectTypesSymbolId =
        "symbol:type:global::AvidScript.ProjectTypes";

    public static bool TryGetKind(
        string canonicalName,
        out CSharpObjectCapabilityKind kind)
    {
        if (HasNominalSuffix(canonicalName, FactoryPrefix))
        {
            kind = CSharpObjectCapabilityKind.Factory;
            return true;
        }
        if (HasNominalSuffix(canonicalName, ObjectTypePrefix))
        {
            kind = CSharpObjectCapabilityKind.ObjectType;
            return true;
        }

        kind = default;
        return false;
    }

    public static bool IsType(SemanticType type)
    {
        return type.Kind == "struct"
            && type.IsValueType
            && TryGetKind(type.CanonicalName, out _);
    }

    public static bool HasCanonicalField(
        SemanticType type,
        IReadOnlyList<SemanticSymbol> symbols)
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
            && callable.HasBody
            && callable.Import is null
            && IsCapabilityTypeId(callable.ContainingTypeId)
            && string.Equals(callable.ReturnTypeId, "type:void", StringComparison.Ordinal)
            && callable.Parameters.Count == 1
            && string.Equals(callable.Parameters[0].TypeId, "type:int32", StringComparison.Ordinal)
            && string.Equals(callable.Parameters[0].RefKind, "none", StringComparison.Ordinal);
    }

    public static bool IsIntrinsicConstructor(
        SemanticDocument document,
        SemanticCallable callable)
    {
        return IsIntrinsicConstructor(callable)
            && IsAuthorizedType(callable.ContainingTypeId, document.Symbols)
            && HasInternalConstructorSymbol(document.Symbols, callable);
    }

    public static bool HasIntrinsicConstructor(
        SemanticType type,
        IReadOnlyList<SemanticSymbol> symbols,
        IReadOnlyList<SemanticCallable> callables)
    {
        return IsAuthorizedType(type.Id, symbols)
            && callables.Count(callable =>
                string.Equals(callable.ContainingTypeId, type.Id, StringComparison.Ordinal)
                && IsIntrinsicConstructor(callable)
                && HasInternalConstructorSymbol(symbols, callable)) == 1;
    }

    public static bool IsOrdinalField(
        SemanticDocument document,
        string? symbolId,
        string? receiverTypeId)
    {
        SemanticSymbol? field = document.Symbols.SingleOrDefault(symbol =>
            string.Equals(symbol.Id, symbolId, StringComparison.Ordinal));
        SemanticType? type = document.Types.SingleOrDefault(item =>
            string.Equals(item.Id, receiverTypeId, StringComparison.Ordinal));
        return field is not null
            && field.Kind == "field"
            && !field.IsStatic
            && string.Equals(
                field.ContainingSymbolId,
                "symbol:" + receiverTypeId,
                StringComparison.Ordinal)
            && type is not null
            && IsType(type)
            && IsAuthorizedType(type.Id, document.Symbols)
            && HasCanonicalField(type, document.Symbols);
    }

    public static bool IsAuthorizedConstruction(
        SemanticDocument document,
        SemanticCallable containingCallable,
        string typeId,
        int ordinal)
    {
        if (ordinal < 0
            || !TryGetKindFromTypeId(typeId, out CSharpObjectCapabilityKind kind)
            || !IsAuthorizedType(typeId, document.Symbols))
        {
            return false;
        }

        SemanticSymbol? property = document.Symbols.SingleOrDefault(symbol =>
            symbol.Kind == "property"
            && symbol.IsStatic
            && string.Equals(symbol.Accessibility, "public", StringComparison.Ordinal)
            && string.Equals(symbol.TypeId, typeId, StringComparison.Ordinal)
            && string.Equals(
                symbol.ContainingSymbolId,
                ProjectContainer(kind),
                StringComparison.Ordinal));
        if (property is null
            || !containingCallable.IsStatic
            || containingCallable.IsConstructor
            || !containingCallable.HasBody
            || containingCallable.Import is not null
            || containingCallable.Parameters.Count != 0
            || !string.Equals(containingCallable.AssociatedSymbolId, property.Id, StringComparison.Ordinal)
            || !string.Equals(containingCallable.ReturnTypeId, typeId, StringComparison.Ordinal))
        {
            return false;
        }

        SemanticControlFlowGraph[] graphs = document.ControlFlowGraphs.Where(graph =>
            string.Equals(
                graph.MethodSymbolId,
                containingCallable.MethodSymbolId,
                StringComparison.Ordinal))
            .ToArray();
        if (graphs.Length != 1)
        {
            return false;
        }

        SemanticOperation[] creations = graphs[0].Blocks
            .SelectMany(block => block.Operations
                .SelectMany(Flatten)
                .Concat(block.BranchValue is null
                    ? Array.Empty<SemanticOperation>()
                    : Flatten(block.BranchValue)))
            .Where(operation => operation.Kind == "object_creation"
                && string.Equals(operation.TypeId, typeId, StringComparison.Ordinal))
            .ToArray();
        return creations.Length == 1
            && TryReadConstructionOrdinal(document, typeId, creations[0], out int publishedOrdinal)
            && publishedOrdinal == ordinal;
    }

    public static bool IsCapabilityTypeId(string? typeId)
    {
        return typeId is not null
            && typeId.StartsWith("type:", StringComparison.Ordinal)
            && TryGetKind(typeId["type:".Length..], out _);
    }

    private static bool TryReadConstructionOrdinal(
        SemanticDocument document,
        string typeId,
        SemanticOperation operation,
        out int ordinal)
    {
        ordinal = -1;
        if (operation.Children.Count != 1
            || !document.Callables.Any(callable =>
                string.Equals(callable.MethodSymbolId, operation.SymbolId, StringComparison.Ordinal)
                && string.Equals(callable.ContainingTypeId, typeId, StringComparison.Ordinal)
                && IsIntrinsicConstructor(document, callable)))
        {
            return false;
        }

        SemanticOperation argument = operation.Children[0].Kind == "argument"
            && operation.Children[0].Children.Count == 1
                ? operation.Children[0].Children[0]
                : operation.Children[0];
        return argument.Kind == "literal"
            && argument.Constant is not null
            && string.Equals(argument.Constant.Kind, "int32", StringComparison.Ordinal)
            && int.TryParse(
                argument.Constant.Value,
                NumberStyles.Integer,
                CultureInfo.InvariantCulture,
                out ordinal)
            && ordinal >= 0;
    }

    private static bool IsAuthorizedType(
        string typeId,
        IReadOnlyList<SemanticSymbol> symbols)
    {
        if (!TryGetKindFromTypeId(typeId, out CSharpObjectCapabilityKind kind))
        {
            return false;
        }

        return symbols.Any(symbol =>
            symbol.Kind == "property"
            && symbol.IsStatic
            && string.Equals(symbol.Accessibility, "public", StringComparison.Ordinal)
            && string.Equals(symbol.TypeId, typeId, StringComparison.Ordinal)
            && string.Equals(
                symbol.ContainingSymbolId,
                ProjectContainer(kind),
                StringComparison.Ordinal));
    }

    private static bool TryGetKindFromTypeId(
        string typeId,
        out CSharpObjectCapabilityKind kind)
    {
        kind = default;
        return typeId.StartsWith("type:", StringComparison.Ordinal)
            && TryGetKind(typeId["type:".Length..], out kind);
    }

    private static string ProjectContainer(CSharpObjectCapabilityKind kind)
    {
        return kind == CSharpObjectCapabilityKind.Factory
            ? ProjectFactoriesSymbolId
            : ProjectTypesSymbolId;
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
            && string.Equals(
                symbol.ContainingSymbolId,
                "symbol:" + callable.ContainingTypeId,
                StringComparison.Ordinal)
            && string.Equals(symbol.Accessibility, "internal", StringComparison.Ordinal);
    }

    private static IEnumerable<SemanticOperation> Flatten(SemanticOperation operation)
    {
        yield return operation;
        foreach (SemanticOperation child in operation.Children)
        {
            foreach (SemanticOperation descendant in Flatten(child))
            {
                yield return descendant;
            }
        }
    }

    private static bool HasNominalSuffix(string canonicalName, string prefix)
    {
        if (!canonicalName.StartsWith(prefix, StringComparison.Ordinal))
        {
            return false;
        }

        ReadOnlySpan<char> suffix = canonicalName.AsSpan(prefix.Length);
        return suffix.Length != 0
            && IsIdentifierStart(suffix[0])
            && suffix[1..].ToString().All(IsIdentifierPart);
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
