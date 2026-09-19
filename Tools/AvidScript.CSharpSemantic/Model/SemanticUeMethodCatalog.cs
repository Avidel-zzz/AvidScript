using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace AvidScript.CSharpSemantic;

// Describes instance-method semantics. This is not a native ABI or an execution permit.
public sealed record SemanticUeMethodCatalog(
    [property: JsonRequired] int SchemaVersion,
    [property: JsonRequired] IReadOnlyList<SemanticUeMethodEntry> Methods,
    [property: JsonRequired] IReadOnlyList<SemanticUeMethodType> Types,
    [property: JsonRequired] IReadOnlyList<SemanticUeInterfaceContract> Interfaces)
{
    public static SemanticUeMethodCatalog Empty => new(1, Array.Empty<SemanticUeMethodEntry>(),
        Array.Empty<SemanticUeMethodType>(), Array.Empty<SemanticUeInterfaceContract>());
}

public sealed record SemanticUeMethodEntry(
    [property: JsonRequired] string MethodSymbolId,
    [property: JsonRequired] string DeclarationMethodSymbolId,
    [property: JsonRequired] string Name,
    [property: JsonRequired] string ContainingTypeId,
    [property: JsonRequired] string SignatureId,
    [property: JsonRequired] string ReturnTypeId,
    [property: JsonRequired] string ReturnRefKind,
    [property: JsonRequired] IReadOnlyList<SemanticUeMethodParameter> Parameters,
    [property: JsonRequired] int GenericArity,
    [property: JsonRequired] string Accessibility,
    [property: JsonRequired] bool HasGuestBody,
    [property: JsonRequired] bool IsReflected,
    [property: JsonRequired] SemanticCallableDispatch Dispatch)
{
    public static string GetConstructedId(string declarationId, string ownerTypeId) => "symbol:ue-method-instance:v1:"
        + Convert.ToHexString(SHA256.HashData(JsonSerializer.SerializeToUtf8Bytes(new[] { declarationId, ownerTypeId }))).ToLowerInvariant();

    public static string GetSignatureId(string returnType, string returnRefKind, int genericArity,
        IReadOnlyList<SemanticUeMethodParameter> parameters)
    {
        using MemoryStream stream = new();
        using (Utf8JsonWriter writer = new(stream))
        {
            writer.WriteStartArray();
            writer.WriteStringValue(returnType);
            writer.WriteStringValue(returnRefKind);
            writer.WriteNumberValue(genericArity);
            foreach (SemanticUeMethodParameter parameter in parameters)
            {
                writer.WriteStartArray();
                writer.WriteNumberValue(parameter.Ordinal);
                writer.WriteStringValue(parameter.TypeId);
                writer.WriteStringValue(parameter.RefKind);
                writer.WriteEndArray();
            }
            writer.WriteEndArray();
        }
        return "ue-method-signature:v1:" + Convert.ToHexString(SHA256.HashData(stream.ToArray())).ToLowerInvariant();
    }
}

public sealed record SemanticUeMethodParameter(
    [property: JsonRequired] int Ordinal,
    [property: JsonRequired] string TypeId,
    [property: JsonRequired] string RefKind);

public sealed record SemanticUeMethodType(
    [property: JsonRequired] string TypeId,
    [property: JsonRequired] IReadOnlyList<string> DeclaredMethodIds,
    [property: JsonRequired] IReadOnlyList<SemanticUeVirtualSlot> VirtualSlots,
    [property: JsonRequired] IReadOnlyList<string> InterfaceTypeIds,
    [property: JsonRequired] IReadOnlyList<SemanticUeInterfaceRoute> InterfaceRoutes);

public sealed record SemanticUeVirtualSlot(
    [property: JsonRequired] string SlotMethodSymbolId,
    [property: JsonRequired] string ImplementationMethodSymbolId);

public sealed record SemanticUeInterfaceContract(
    [property: JsonRequired] string TypeId,
    [property: JsonRequired] IReadOnlyList<string> BaseInterfaceTypeIds,
    [property: JsonRequired] IReadOnlyList<string> MethodIds);

public sealed record SemanticUeInterfaceRoute(
    [property: JsonRequired] string InterfaceMethodSymbolId,
    [property: JsonRequired] string? ImplementationMethodSymbolId,
    [property: JsonRequired] string Kind);
