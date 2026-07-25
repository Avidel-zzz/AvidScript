using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using AvidScript.CSharpSemantic;

namespace AvidScript.CSharpGuest;

internal static class CSharpClassReferencePolicy
{
    public const string CanonicalPrefix = "global::AvidScript.TSubclassOf";
    public const string CanonicalName = "global::AvidScript.TSubclassOfAActor";
    public const string OrdinalFieldName = "Ordinal";
    private const string ProjectClassesSymbolId =
        "symbol:type:global::AvidScript.ProjectClasses";
    private const string UeSymbolId =
        "symbol:type:global::AvidScript.UE";

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
            && callable.HasBody
            && callable.Import is null
            && IsClassReferenceTypeId(callable.ContainingTypeId)
            && string.Equals(callable.ReturnTypeId, "type:void", StringComparison.Ordinal)
            && callable.Parameters.Count == 1
            && string.Equals(callable.Parameters[0].TypeId, "type:int32", StringComparison.Ordinal)
            && string.Equals(callable.Parameters[0].RefKind, "none", StringComparison.Ordinal);
    }

    public static bool IsIntrinsicConstructor(SemanticDocument document, SemanticCallable callable)
    {
        return IsIntrinsicConstructor(document, callable.ContainingTypeId, callable);
    }

    public static bool IsIntrinsicConstructor(
        SemanticDocument document,
        string? expectedTypeId,
        SemanticCallable callable)
    {
        if (!string.Equals(callable.ContainingTypeId, expectedTypeId, StringComparison.Ordinal)
            || !IsIntrinsicConstructor(callable)
            || !IsAuthorizedType(callable.ContainingTypeId, document.Symbols))
        {
            return false;
        }

        return HasInternalSymbol(document.Symbols, callable);
    }

    public static bool HasIntrinsicConstructor(
        SemanticType type,
        System.Collections.Generic.IReadOnlyList<SemanticSymbol> symbols,
        System.Collections.Generic.IReadOnlyList<SemanticCallable> callables)
    {
        return IsAuthorizedType(type.Id, symbols)
            && callables.Count(callable =>
            string.Equals(callable.ContainingTypeId, type.Id, StringComparison.Ordinal)
            && IsIntrinsicConstructor(callable)
            && HasInternalSymbol(symbols, callable)) == 1;
    }

    public static bool IsIntrinsicUpcast(
        SemanticDocument document,
        SemanticCallable callable)
    {
        if (!callable.IsStatic
            || callable.IsConstructor
            || !callable.HasBody
            || callable.Import is not null
            || callable.Parameters.Count != 1
            || !string.Equals(callable.Parameters[0].RefKind, "none", StringComparison.Ordinal)
            || !string.Equals(
                callable.ContainingTypeId,
                callable.Parameters[0].TypeId,
                StringComparison.Ordinal)
            || string.Equals(
                callable.Parameters[0].TypeId,
                callable.ReturnTypeId,
                StringComparison.Ordinal)
            || !IsClassReferenceTypeId(callable.Parameters[0].TypeId)
            || !IsClassReferenceTypeId(callable.ReturnTypeId))
        {
            return false;
        }

        SemanticSymbol? symbol = document.Symbols.SingleOrDefault(item =>
            string.Equals(item.Id, callable.MethodSymbolId, StringComparison.Ordinal));
        SemanticType? sourceType = document.Types.SingleOrDefault(type =>
            string.Equals(type.Id, callable.Parameters[0].TypeId, StringComparison.Ordinal));
        SemanticType? targetType = document.Types.SingleOrDefault(type =>
            string.Equals(type.Id, callable.ReturnTypeId, StringComparison.Ordinal));
        return symbol is not null
            && symbol.Kind == "method"
            && symbol.Name == "op_Implicit"
            && symbol.IsStatic
            && symbol.Accessibility == "public"
            && string.Equals(
                symbol.ContainingSymbolId,
                "symbol:" + callable.ContainingTypeId,
                StringComparison.Ordinal)
            && sourceType is not null
            && targetType is not null
            && IsType(sourceType)
            && IsType(targetType)
            && IsAuthorizedType(sourceType.Id, document.Symbols)
            && IsAuthorizedType(targetType.Id, document.Symbols)
            && HasCanonicalField(sourceType, document.Symbols)
            && HasCanonicalField(targetType, document.Symbols);
    }

    public static bool IsOrdinalField(
        SemanticDocument document,
        string? symbolId,
        string? receiverTypeId)
    {
        SemanticSymbol? field = document.Symbols.SingleOrDefault(symbol =>
            string.Equals(symbol.Id, symbolId, StringComparison.Ordinal));
        return field is not null
            && field.Kind == "field"
            && !field.IsStatic
            && string.Equals(
                field.ContainingSymbolId,
                "symbol:" + receiverTypeId,
                StringComparison.Ordinal)
            && document.Types.SingleOrDefault(type =>
                string.Equals(type.Id, receiverTypeId, StringComparison.Ordinal)) is { } type
            && IsType(type)
            && IsAuthorizedType(type.Id, document.Symbols)
            && HasCanonicalField(type, document.Symbols);
    }

    public static bool IsClassReferenceTypeId(string? typeId)
    {
        return typeId is not null
            && typeId.StartsWith("type:", StringComparison.Ordinal)
            && IsCanonicalName(typeId["type:".Length..]);
    }

    public static bool IsAuthorizedOrdinal(
        SemanticDocument document,
        string typeId,
        int ordinal)
    {
        if (!IsClassReferenceTypeId(typeId) || ordinal < 0)
        {
            return false;
        }

        foreach (SemanticSymbol property in document.Symbols.Where(symbol =>
            symbol.Kind == "property"
            && symbol.IsStatic
            && string.Equals(symbol.Accessibility, "public", StringComparison.Ordinal)
            && string.Equals(symbol.ContainingSymbolId, ProjectClassesSymbolId, StringComparison.Ordinal)
            && IsClassReferenceTypeId(symbol.TypeId)))
        {
            if (TryReadPublishedOrdinal(document, property, out int publishedOrdinal)
                && publishedOrdinal == ordinal
                && IsCompatibleClassReferenceType(document, property.TypeId!, typeId))
            {
                return true;
            }
        }
        return false;
    }

    private static bool TryReadPublishedOrdinal(
        SemanticDocument document,
        SemanticSymbol property,
        out int ordinal)
    {
        ordinal = -1;
        SemanticCallable[] getters = document.Callables.Where(callable =>
            callable.IsStatic
            && !callable.IsConstructor
            && callable.HasBody
            && callable.Import is null
            && string.Equals(callable.AssociatedSymbolId, property.Id, StringComparison.Ordinal)
            && string.Equals(callable.ReturnTypeId, property.TypeId, StringComparison.Ordinal))
            .ToArray();
        if (getters.Length != 1)
        {
            return false;
        }

        SemanticControlFlowGraph[] graphs = document.ControlFlowGraphs.Where(graph =>
            string.Equals(graph.MethodSymbolId, getters[0].MethodSymbolId, StringComparison.Ordinal))
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
            .Where(operation =>
                operation.Kind == "object_creation"
                && string.Equals(operation.TypeId, property.TypeId, StringComparison.Ordinal))
            .ToArray();
        if (creations.Length != 1
            || creations[0].Children.Count != 1
            || !document.Callables.Any(callable =>
                string.Equals(callable.MethodSymbolId, creations[0].SymbolId, StringComparison.Ordinal)
                && IsIntrinsicConstructor(document, property.TypeId, callable)))
        {
            return false;
        }

        SemanticOperation argument = creations[0].Children[0].Kind == "argument"
            && creations[0].Children[0].Children.Count == 1
                ? creations[0].Children[0].Children[0]
                : creations[0].Children[0];
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

    private static bool IsCompatibleClassReferenceType(
        SemanticDocument document,
        string publishedTypeId,
        string requestedTypeId)
    {
        if (string.Equals(publishedTypeId, requestedTypeId, StringComparison.Ordinal))
        {
            return true;
        }

        HashSet<string> visited = new(StringComparer.Ordinal) { publishedTypeId };
        Queue<string> pending = new();
        pending.Enqueue(publishedTypeId);
        while (pending.Count != 0)
        {
            string currentTypeId = pending.Dequeue();
            foreach (SemanticCallable upcast in document.Callables.Where(callable =>
                callable.Parameters.Count == 1
                && string.Equals(callable.Parameters[0].TypeId, currentTypeId, StringComparison.Ordinal)
                && IsIntrinsicUpcast(document, callable)))
            {
                if (string.Equals(upcast.ReturnTypeId, requestedTypeId, StringComparison.Ordinal))
                {
                    return true;
                }
                if (visited.Add(upcast.ReturnTypeId))
                {
                    pending.Enqueue(upcast.ReturnTypeId);
                }
            }
        }
        return false;
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

    private static bool IsAuthorizedType(
        string typeId,
        System.Collections.Generic.IReadOnlyList<SemanticSymbol> symbols)
    {
        if (!IsClassReferenceTypeId(typeId))
        {
            return false;
        }

        bool isPublishedProjectClass = symbols.Any(symbol =>
            symbol.Kind == "property"
            && symbol.IsStatic
            && string.Equals(symbol.Accessibility, "public", StringComparison.Ordinal)
            && string.Equals(symbol.ContainingSymbolId, ProjectClassesSymbolId, StringComparison.Ordinal)
            && string.Equals(symbol.TypeId, typeId, StringComparison.Ordinal));
        if (isPublishedProjectClass)
        {
            return true;
        }

        System.Collections.Generic.HashSet<string> spawnMethodIds = symbols
            .Where(symbol =>
                symbol.Kind == "method"
                && symbol.Name == "SpawnActor"
                && symbol.IsStatic
                && string.Equals(symbol.Accessibility, "public", StringComparison.Ordinal)
                && string.Equals(symbol.ContainingSymbolId, UeSymbolId, StringComparison.Ordinal))
            .Select(symbol => symbol.Id)
            .ToHashSet(StringComparer.Ordinal);
        return symbols.Any(symbol =>
            symbol.Kind == "parameter"
            && symbol.Name == "actorClass"
            && string.Equals(symbol.TypeId, typeId, StringComparison.Ordinal)
            && symbol.ContainingSymbolId is not null
            && spawnMethodIds.Contains(symbol.ContainingSymbolId));
    }

    private static bool IsIdentifierStart(char value)
    {
        return value == '_' || char.IsLetter(value);
    }

    private static bool IsIdentifierPart(char value)
    {
        return value == '_' || char.IsLetterOrDigit(value);
    }

    private static bool HasInternalSymbol(
        System.Collections.Generic.IReadOnlyList<SemanticSymbol> symbols,
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
}
