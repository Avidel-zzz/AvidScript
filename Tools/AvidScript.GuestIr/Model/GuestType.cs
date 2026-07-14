using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AvidScript.GuestIr;

public sealed record GuestType(
    [property: JsonPropertyOrder(0)] string Id,
    [property: JsonPropertyOrder(1)] string Kind,
    [property: JsonPropertyOrder(2)] string Storage,
    [property: JsonPropertyOrder(3)] IReadOnlyList<GuestField> Fields,
    [property: JsonPropertyOrder(4)] string? ElementTypeId,
    [property: JsonPropertyOrder(5)] string? UnderlyingTypeId,
    [property: JsonPropertyOrder(6)] int Size,
    [property: JsonPropertyOrder(7)] int Alignment);

public sealed record GuestField(
    [property: JsonPropertyOrder(0)] string Id,
    [property: JsonPropertyOrder(1)] string Name,
    [property: JsonPropertyOrder(2)] string TypeId,
    [property: JsonPropertyOrder(3)] int Offset);
