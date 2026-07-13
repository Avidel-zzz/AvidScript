using System.Text.Json.Serialization;

namespace AvidScript.CSharpSemantic;

public sealed record SemanticDiagnostic(
    [property: JsonPropertyOrder(0)] string Code,
    [property: JsonPropertyOrder(1)] string Severity,
    [property: JsonPropertyOrder(2)] string Message,
    [property: JsonPropertyOrder(3)] SemanticSpan Span);
