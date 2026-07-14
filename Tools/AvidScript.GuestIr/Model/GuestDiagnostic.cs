using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AvidScript.GuestIr;

public sealed record GuestValidationResult(
    bool Succeeded,
    IReadOnlyList<GuestDiagnostic> Diagnostics);

public sealed record GuestDiagnostic(
    [property: JsonPropertyOrder(0)] string Code,
    [property: JsonPropertyOrder(1)] string Severity,
    [property: JsonPropertyOrder(2)] string Message,
    [property: JsonPropertyOrder(3)] GuestSourceSpan? Span);

public sealed record GuestSourceSpan(
    [property: JsonPropertyOrder(0)] int Start,
    [property: JsonPropertyOrder(1)] int Length,
    [property: JsonPropertyOrder(2)] int StartLine,
    [property: JsonPropertyOrder(3)] int StartColumn,
    [property: JsonPropertyOrder(4)] int EndLine,
    [property: JsonPropertyOrder(5)] int EndColumn);
