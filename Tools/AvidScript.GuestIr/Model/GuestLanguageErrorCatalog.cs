using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AvidScript.GuestIr;

// IR 17 owns the source/type tokens carried by a language outcome. Source
// offsets and line/column coordinates are zero-based UTF-16 source positions.
public sealed record GuestLanguageErrorCatalog(
    [property: JsonPropertyOrder(0)] IReadOnlyList<GuestLanguageErrorTypeToken> Types,
    [property: JsonPropertyOrder(1)] IReadOnlyList<GuestLanguageErrorSourceToken> Sources)
{
    public const int SchemaVersion = 17;
    public const string IrVersion = "1.16";
}

public sealed record GuestLanguageErrorTypeToken(
    [property: JsonPropertyOrder(0)] int Token,
    [property: JsonPropertyOrder(1)] string TypeId);

public sealed record GuestLanguageErrorSourceToken(
    [property: JsonPropertyOrder(0)] int Token,
    [property: JsonPropertyOrder(1)] string SourceId,
    [property: JsonPropertyOrder(2)] int SourceLength,
    [property: JsonPropertyOrder(3)] int Start,
    [property: JsonPropertyOrder(4)] int Length,
    [property: JsonPropertyOrder(5)] int Line,
    [property: JsonPropertyOrder(6)] int Column,
    [property: JsonPropertyOrder(7)] int EndLine,
    [property: JsonPropertyOrder(8)] int EndColumn);
