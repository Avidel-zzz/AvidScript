using System.Text.Json.Serialization;

namespace AvidScript.CSharpSemantic;

public sealed record SemanticSource(
    [property: JsonPropertyOrder(0)] string SourceId,
    [property: JsonPropertyOrder(1)] string Sha256,
    [property: JsonPropertyOrder(2)] string FrontendSha256,
    [property: JsonPropertyOrder(3)] int Length);
