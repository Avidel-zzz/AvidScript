using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AvidScript.GuestIr;

public sealed record GuestFunction(
    [property: JsonPropertyOrder(0)] string Id,
    [property: JsonPropertyOrder(1)] IReadOnlyList<GuestRegister> Parameters,
    [property: JsonPropertyOrder(2)] IReadOnlyList<GuestRegister> Locals,
    [property: JsonPropertyOrder(3)] string ReturnTypeId,
    [property: JsonPropertyOrder(4)] string EntryBlockId,
    [property: JsonPropertyOrder(5)] IReadOnlyList<GuestBasicBlock> Blocks);

public sealed record GuestRegister(
    [property: JsonPropertyOrder(0)] string Id,
    [property: JsonPropertyOrder(1)] string TypeId);

public sealed record GuestBasicBlock(
    [property: JsonPropertyOrder(0)] string Id,
    [property: JsonPropertyOrder(1)] IReadOnlyList<GuestInstruction> Instructions,
    [property: JsonPropertyOrder(2)] GuestTerminator Terminator);
