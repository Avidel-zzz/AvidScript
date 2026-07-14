using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AvidScript.GuestIr;

public sealed record GuestDataSegment(
    [property: JsonPropertyOrder(0)] string Id,
    [property: JsonPropertyOrder(1)] string Kind,
    [property: JsonPropertyOrder(2)] string TypeId,
    [property: JsonPropertyOrder(3)] int Address,
    [property: JsonPropertyOrder(4)] int Alignment,
    [property: JsonPropertyOrder(5)] int ElementCount,
    [property: JsonPropertyOrder(6)] IReadOnlyList<byte> Bytes);

public sealed record GuestStateSlot(
    [property: JsonPropertyOrder(0)] string GlobalId,
    [property: JsonPropertyOrder(1)] string TypeId,
    [property: JsonPropertyOrder(2)] int Offset,
    [property: JsonPropertyOrder(3)] int Size,
    [property: JsonPropertyOrder(4)] int Alignment);

public sealed record GuestMemoryLayout(
    [property: JsonPropertyOrder(0)] int StateStart,
    [property: JsonPropertyOrder(1)] int StateSize,
    [property: JsonPropertyOrder(2)] int DataStart,
    [property: JsonPropertyOrder(3)] int DataEnd,
    [property: JsonPropertyOrder(4)] int HeapStart,
    [property: JsonPropertyOrder(5)] IReadOnlyList<GuestStateSlot> StateSlots);
