using System;
using System.Collections.Generic;
using System.Linq;
using Microsoft.CodeAnalysis;

namespace AvidScript.CSharpSemantic;

internal sealed class SemanticTypeRegistry
{
    private readonly Dictionary<string, SemanticType> types = new(StringComparer.Ordinal);
    private readonly Dictionary<string, SemanticTypeShape> shapes = new(StringComparer.Ordinal);
    private readonly Dictionary<string, SemanticDelegateType> delegateTypes = new(StringComparer.Ordinal);
    private readonly Dictionary<string, SemanticClassType> classTypes = new(StringComparer.Ordinal);
    private readonly Dictionary<string, ITypeSymbol> sourceTypes = new(StringComparer.Ordinal);

    internal bool StaticFieldOwners { get; init; }
    internal Compilation? Compilation { get; init; }

    public string Register(ITypeSymbol type)
    {
        string canonicalName = GetCanonicalName(type);
        string id = "type:" + canonicalName;
        sourceTypes.TryAdd(id, type);
        if (!types.TryAdd(id, new SemanticType(
            id,
            canonicalName,
            type.ToDisplayString(SymbolDisplayFormat.MinimallyQualifiedFormat),
            GetKind(type),
            type.IsValueType,
            type.NullableAnnotation == NullableAnnotation.Annotated))) return id;
        if (type is IArrayTypeSymbol array)
        {
            shapes.TryAdd(id, new SemanticTypeShape(id, Register(array.ElementType), null));
        }
        else if (type is INamedTypeSymbol { TypeKind: TypeKind.Enum, EnumUnderlyingType: { } underlying })
        {
            shapes.TryAdd(id, new SemanticTypeShape(id, null, Register(underlying)));
        }
        else if (type is INamedTypeSymbol { TypeKind: TypeKind.Delegate, DelegateInvokeMethod: { } invoke })
        {
            SemanticDelegateParameter[] parameters = invoke.Parameters.Select(parameter =>
                new SemanticDelegateParameter(parameter.Ordinal, Register(parameter.Type),
                    GetDelegateRefKind(parameter.RefKind))).ToArray();
            string returnTypeId = Register(invoke.ReturnType);
            string returnRefKind = invoke.ReturnsByRefReadonly ? "ref_readonly" : invoke.ReturnsByRef ? "ref" : "none";
            delegateTypes.Add(id, new(id,
                SemanticDelegateType.GetInvokeId(id, returnTypeId, returnRefKind, parameters),
                returnTypeId, returnRefKind, parameters));
        }
        else if (type is INamedTypeSymbol named
            && named.IsGenericType
            && named.Arity == 1
            && named.Name == "AvidOutcome"
            && named.ContainingNamespace.ToDisplayString() == "AvidScript")
        {
            shapes.TryAdd(id, new SemanticTypeShape(
                id,
                null,
                null,
                Register(named.TypeArguments[0])));
        }
        if (type is INamedTypeSymbol genericType && genericType.IsGenericType)
        {
            string definitionTypeId = "type:" + GetCanonicalName(genericType.OriginalDefinition);
            string[] argumentTypeIds = genericType.TypeArguments
                .Select(Register).ToArray();
            shapes[id] = shapes.TryGetValue(id, out SemanticTypeShape? existing)
                ? existing with
                {
                    GenericDefinitionTypeId = definitionTypeId,
                    GenericArgumentTypeIds = argumentTypeIds,
                }
                : new SemanticTypeShape(id, null, null, null,
                    definitionTypeId, argumentTypeIds);
        }
        if (type is INamedTypeSymbol { TypeKind: TypeKind.Class } classType && GetKind(type) == "class")
            classTypes.Add(id, SemanticClassTypeProjector.Project(classType, id, this));
        return id;
    }

    // Construct through Roslyn, never by replacing fragments of a printed name.
    // This also registers closed types used only inside a generic method body.
    internal bool TryClose(string typeId, IReadOnlyDictionary<string, string> arguments, out string closedId)
    {
        closedId = typeId;
        if (!sourceTypes.TryGetValue(typeId, out var source)) return false;
        ITypeSymbol? Substitute(ITypeSymbol type, int depth)
        {
            if (depth > 32) return null;
            string id = "type:" + GetCanonicalName(type);
            if (arguments.TryGetValue(id, out var replacement))
                return sourceTypes.GetValueOrDefault(replacement);
            if (type is ITypeParameterSymbol) return null;
            if (type is IArrayTypeSymbol array)
                return Substitute(array.ElementType, depth + 1) is { } element
                    ? Compilation?.CreateArrayTypeSymbol(element, array.Rank) : null;
            if (type is not INamedTypeSymbol named) return type;
            INamedTypeSymbol definition = named.OriginalDefinition;
            if (named.ContainingType is { } parent)
            {
                if (Substitute(parent, depth + 1) is not INamedTypeSymbol closedParent) return null;
                definition = closedParent.GetTypeMembers(named.Name, named.Arity).SingleOrDefault()!;
                if (definition is null) return null;
            }
            if (named.Arity == 0) return definition;
            ITypeSymbol[] closedArguments = new ITypeSymbol[named.TypeArguments.Length];
            for (int index = 0; index < closedArguments.Length; ++index)
            {
                if (Substitute(named.TypeArguments[index], depth + 1) is not { } argument) return null;
                closedArguments[index] = argument;
            }
            return definition.Construct(closedArguments);
        }
        if (Substitute(source, 0) is not { } closed) return false;
        closedId = Register(closed);
        return true;
    }

    public IReadOnlyList<SemanticType> Build()
    {
        return types.Values.OrderBy(type => type.Id, StringComparer.Ordinal).ToArray();
    }

    public IReadOnlyList<SemanticTypeShape> BuildShapes()
    {
        return shapes.Values.OrderBy(shape => shape.TypeId, StringComparer.Ordinal).ToArray();
    }

    public IReadOnlyList<SemanticDelegateType> BuildDelegateTypes() =>
        delegateTypes.Values.OrderBy(type => type.TypeId, StringComparer.Ordinal).ToArray();

    public IReadOnlyList<SemanticClassType> BuildClassTypes() =>
        classTypes.Values.OrderBy(type => type.TypeId, StringComparer.Ordinal).ToArray();

    internal static string GetDelegateRefKind(RefKind kind) => kind switch
    {
        RefKind.None => "none",
        RefKind.Ref => "ref",
        RefKind.Out => "out",
        RefKind.In => "in",
        RefKind.RefReadOnlyParameter => "ref_readonly",
        _ => "unsupported:" + kind,
    };

    public static string GetCanonicalName(ITypeSymbol type)
    {
        if (type is IArrayTypeSymbol array)
        {
            return GetCanonicalName(array.ElementType) + "[" + new string(',', array.Rank - 1) + "]";
        }

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
        if (type.SpecialType is
            SpecialType.System_Void or
            SpecialType.System_Boolean or
            SpecialType.System_Byte or
            SpecialType.System_SByte or
            SpecialType.System_Int16 or
            SpecialType.System_UInt16 or
            SpecialType.System_Int32 or
            SpecialType.System_UInt32 or
            SpecialType.System_Int64 or
            SpecialType.System_UInt64 or
            SpecialType.System_Single or
            SpecialType.System_Double or
            SpecialType.System_Char or
            SpecialType.System_String)
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
