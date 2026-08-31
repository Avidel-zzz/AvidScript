using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;

namespace AvidScript.UeTypeGenerator;

internal sealed class UeCppTypeMapper
{
    private static readonly IReadOnlyDictionary<string, string> PrimitiveTypes =
        new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["void"] = "void",
            ["bool"] = "bool",
            ["uint8"] = "uint8",
            ["int8"] = "int8",
            ["int16"] = "int16",
            ["uint16"] = "uint16",
            ["int32"] = "int32",
            ["uint32"] = "uint32",
            ["int64"] = "int64",
            ["uint64"] = "uint64",
            ["float32"] = "float",
            ["float64"] = "double",
            ["string"] = "FString",
        };

    private static readonly IReadOnlyDictionary<string, string> ValueTypes =
        new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["global::AvidScript.FVector"] = "FVector",
            ["global::AvidScript.FVector2D"] = "FVector2D",
            ["global::AvidScript.FRotator"] = "FRotator",
            ["global::AvidScript.FTransform"] = "FTransform",
            ["global::AvidScript.FQuat"] = "FQuat",
            ["global::AvidScript.FLinearColor"] = "FLinearColor",
            ["global::AvidScript.FColor"] = "FColor",
        };

    private static readonly IReadOnlyDictionary<string, string> ObjectTypes =
        new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["global::AvidScript.UObject"] = "UObject",
            ["global::AvidScript.AActor"] = "AActor",
            ["global::AvidScript.APawn"] = "APawn",
            ["global::AvidScript.ACharacter"] = "ACharacter",
            ["global::AvidScript.UActorComponent"] = "UActorComponent",
            ["global::AvidScript.USceneComponent"] = "USceneComponent",
            ["global::AvidScript.UPrimitiveComponent"] = "UPrimitiveComponent",
        };

    private readonly IReadOnlyDictionary<string, SemanticType> types;
    private readonly IReadOnlyDictionary<string, SemanticTypeShape> shapes;
    private readonly IReadOnlyDictionary<string, string> scriptCppNames;

    public UeCppTypeMapper(
        IReadOnlyList<SemanticType> semanticTypes,
        IReadOnlyList<SemanticTypeShape> semanticShapes,
        IReadOnlyDictionary<string, string> scriptCppNames)
    {
        types = semanticTypes.ToDictionary(type => type.Id, StringComparer.Ordinal);
        shapes = semanticShapes.ToDictionary(shape => shape.TypeId, StringComparer.Ordinal);
        this.scriptCppNames = scriptCppNames;
    }

    public string MapProperty(string typeId)
    {
        return Map(typeId, true, new HashSet<string>(StringComparer.Ordinal));
    }

    public string MapCallable(string typeId, string refKind = "none")
    {
        string mapped = Map(typeId, false, new HashSet<string>(StringComparer.Ordinal));
        return refKind switch
        {
            "none" => mapped,
            "ref" or "out" or "in" when mapped != "void" => mapped + "&",
            _ => throw new InvalidOperationException($"Unsupported callable ref kind '{refKind}' for '{typeId}'."),
        };
    }

    private string Map(string typeId, bool propertyContext, ISet<string> visiting)
    {
        if (!types.TryGetValue(typeId, out SemanticType? type))
        {
            throw new InvalidOperationException($"Semantic type '{typeId}' is missing.");
        }
        if (type.IsNullable)
        {
            throw new InvalidOperationException($"Nullable UE shell type '{type.CanonicalName}' is not supported in P59.B.");
        }
        if (PrimitiveTypes.TryGetValue(type.CanonicalName, out string? primitive))
        {
            return primitive;
        }
        if (ValueTypes.TryGetValue(type.CanonicalName, out string? valueType))
        {
            return valueType;
        }
        if (scriptCppNames.TryGetValue(typeId, out string? scriptType))
        {
            return propertyContext ? $"TObjectPtr<{scriptType}>" : scriptType + "*";
        }
        if (ObjectTypes.TryGetValue(type.CanonicalName, out string? objectType))
        {
            return propertyContext ? $"TObjectPtr<{objectType}>" : objectType + "*";
        }
        const string FacadePrefix = "global::AvidScript.";
        if (type.Kind == "enum" && type.CanonicalName.StartsWith(FacadePrefix, StringComparison.Ordinal))
        {
            string enumName = type.CanonicalName[FacadePrefix.Length..];
            if (IsIdentifier(enumName))
            {
                return enumName;
            }
        }
        if (type.Kind == "array"
            && shapes.TryGetValue(typeId, out SemanticTypeShape? shape)
            && !string.IsNullOrWhiteSpace(shape.ElementTypeId))
        {
            if (!visiting.Add(typeId))
            {
                throw new InvalidOperationException($"Recursive UE shell container type '{typeId}' is not supported.");
            }
            string elementType = Map(shape.ElementTypeId, false, visiting);
            visiting.Remove(typeId);
            return $"TArray<{elementType}>";
        }
        throw new InvalidOperationException(
            $"Semantic type '{type.CanonicalName}' has no deterministic UE shell mapping in P59.B.");
    }

    private static bool IsIdentifier(string value)
    {
        return value.Length > 0
            && (value[0] == '_' || value[0] is >= 'A' and <= 'Z' or >= 'a' and <= 'z')
            && value.Skip(1).All(character =>
                character == '_' || character is >= 'A' and <= 'Z' or >= 'a' and <= 'z' or >= '0' and <= '9');
    }
}
