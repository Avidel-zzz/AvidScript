using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AvidScript.CSharpSemantic;

// These are source-language facts, not a promise that Guest lowering supports the class.
public sealed record SemanticClassType(
    [property: JsonPropertyOrder(0), JsonRequired] string TypeId,
    [property: JsonPropertyOrder(1), JsonRequired] string? BaseTypeId,
    [property: JsonPropertyOrder(2), JsonRequired] IReadOnlyList<string> InterfaceTypeIds,
    [property: JsonPropertyOrder(3), JsonRequired] bool IsSourceDeclared,
    [property: JsonPropertyOrder(4), JsonRequired] bool IsStatic,
    [property: JsonPropertyOrder(5), JsonRequired] bool IsAbstract,
    [property: JsonPropertyOrder(6), JsonRequired] bool IsSealed,
    [property: JsonPropertyOrder(7), JsonRequired] bool IsGeneric,
    [property: JsonPropertyOrder(8), JsonRequired] bool IsRecord,
    [property: JsonPropertyOrder(9), JsonRequired] bool HasPrimaryConstructor,
    [property: JsonPropertyOrder(10), JsonRequired] bool HasImplicitDefaultConstructor,
    [property: JsonPropertyOrder(11), JsonRequired] bool HasInstanceInitializers,
    [property: JsonPropertyOrder(12), JsonRequired] bool HasStaticInitialization,
    [property: JsonPropertyOrder(13), JsonRequired] bool HasImplicitInstanceStorage,
    [property: JsonPropertyOrder(14), JsonRequired] bool HasVirtualMembers,
    [property: JsonPropertyOrder(15), JsonRequired] bool HasFinalizer);
