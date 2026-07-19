using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AvidScript.CSharpGuest;

public sealed record CSharpGuestStateSchema(
    [property: JsonPropertyOrder(0)] int SchemaVersion,
    [property: JsonPropertyOrder(1)] string Strategy,
    [property: JsonPropertyOrder(2)] string Policy,
    [property: JsonPropertyOrder(3)] int ContractVersion,
    [property: JsonPropertyOrder(4)] string OwnerTypeId,
    [property: JsonPropertyOrder(5)] IReadOnlyList<CSharpGuestStateSlot> Slots);

public sealed record CSharpGuestStateSlot(
    [property: JsonPropertyOrder(0)] string StableId,
    [property: JsonPropertyOrder(1)] IReadOnlyList<string> Aliases,
    [property: JsonPropertyOrder(2)] string TypeFingerprint,
    [property: JsonPropertyOrder(3)] int Offset,
    [property: JsonPropertyOrder(4)] int Size,
    [property: JsonPropertyOrder(5)] int Alignment);
