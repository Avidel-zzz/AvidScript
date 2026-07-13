using System;
using System.Collections.Generic;
using System.Linq;
using Microsoft.CodeAnalysis;

namespace AvidScript.CSharpSemantic;

internal sealed class SemanticTypeRegistry
{
    private readonly Dictionary<string, SemanticType> types = new(StringComparer.Ordinal);

    public string Register(ITypeSymbol type)
    {
        string canonicalName = GetCanonicalName(type);
        string id = "type:" + canonicalName;
        types.TryAdd(id, new SemanticType(
            id,
            canonicalName,
            type.ToDisplayString(SymbolDisplayFormat.MinimallyQualifiedFormat),
            GetKind(type),
            type.IsValueType,
            type.NullableAnnotation == NullableAnnotation.Annotated));
        return id;
    }

    public IReadOnlyList<SemanticType> Build()
    {
        return types.Values.OrderBy(type => type.Id, StringComparer.Ordinal).ToArray();
    }

    public static string GetCanonicalName(ITypeSymbol type)
    {
        return type.SpecialType switch
        {
            SpecialType.System_Void => "void",
            SpecialType.System_Boolean => "bool",
            SpecialType.System_Byte => "uint8",
            SpecialType.System_SByte => "int8",
            SpecialType.System_Int16 => "int16",
            SpecialType.System_UInt16 => "uint16",
            SpecialType.System_Int32 => "int32",
            SpecialType.System_UInt32 => "uint32",
            SpecialType.System_Int64 => "int64",
            SpecialType.System_UInt64 => "uint64",
            SpecialType.System_Single => "float32",
            SpecialType.System_Double => "float64",
            SpecialType.System_Char => "char16",
            SpecialType.System_String => "string",
            SpecialType.System_Object => "object",
            _ => type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat),
        };
    }

    private static string GetKind(ITypeSymbol type)
    {
        if (type.SpecialType != SpecialType.None)
        {
            return "primitive";
        }

        return type.TypeKind switch
        {
            TypeKind.Array => "array",
            TypeKind.Class => "class",
            TypeKind.Delegate => "delegate",
            TypeKind.Enum => "enum",
            TypeKind.Interface => "interface",
            TypeKind.Struct => "struct",
            TypeKind.TypeParameter => "type_parameter",
            TypeKind.Error => "error",
            _ => type.TypeKind.ToString().ToLowerInvariant(),
        };
    }
}
