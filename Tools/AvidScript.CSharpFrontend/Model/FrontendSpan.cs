using System.Text.Json.Serialization;

namespace AvidScript.CSharpFrontend;

public sealed record FrontendSpan(
    [property: JsonPropertyOrder(0)] int Start,
    [property: JsonPropertyOrder(1)] int Length,
    [property: JsonPropertyOrder(2)] int End,
    [property: JsonPropertyOrder(3)] int Line,
    [property: JsonPropertyOrder(4)] int Column,
    [property: JsonPropertyOrder(5)] int EndLine,
    [property: JsonPropertyOrder(6)] int EndColumn);
