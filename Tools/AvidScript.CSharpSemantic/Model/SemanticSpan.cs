using System.Text.Json.Serialization;

namespace AvidScript.CSharpSemantic;

public sealed record SemanticSpan(
    [property: JsonPropertyOrder(0)] int Start,
    [property: JsonPropertyOrder(1)] int Length,
    [property: JsonPropertyOrder(2)] int Line,
    [property: JsonPropertyOrder(3)] int Column,
    [property: JsonPropertyOrder(4)] int EndLine,
    [property: JsonPropertyOrder(5)] int EndColumn)
{
    [JsonIgnore]
    public int End => Start + Length;
}
