using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AvidScript.GuestIr;

public sealed record GuestInstruction(
    [property: JsonPropertyOrder(0)] string Op,
    [property: JsonPropertyOrder(1)] string? ResultId,
    [property: JsonPropertyOrder(2)] IReadOnlyList<string> OperandIds,
    [property: JsonPropertyOrder(3)] string? TargetId,
    [property: JsonPropertyOrder(4)] string? OperatorKind,
    [property: JsonPropertyOrder(5)] GuestConstant? Constant,
    [property: JsonIgnore] GuestDebugLocation? DebugLocation = null);

public sealed record GuestTerminator(
    [property: JsonPropertyOrder(0)] string Kind,
    [property: JsonPropertyOrder(1)] string? ConditionValueId,
    [property: JsonPropertyOrder(2)] string? TargetBlockId,
    [property: JsonPropertyOrder(3)] string? FalseTargetBlockId,
    [property: JsonPropertyOrder(4)] string? ReturnValueId,
    [property: JsonIgnore] GuestDebugLocation? DebugLocation = null);

public sealed record GuestConstant(
    [property: JsonPropertyOrder(0)] string Kind,
    [property: JsonPropertyOrder(1)] string? Value);

public sealed record GuestDebugLocation(
    string SemanticOperationId,
    string Kind,
    bool Hidden,
    int Start,
    int Length,
    int Line,
    int Column,
    int EndLine,
    int EndColumn);

public static class GuestDebugIdentity
{
    public static string Instruction(string functionId, string blockId, int instructionIndex)
    {
        return $"{functionId}@{blockId}:instruction:{instructionIndex}";
    }

    public static string Terminator(string functionId, string blockId)
    {
        return $"{functionId}@{blockId}:terminator";
    }
}
