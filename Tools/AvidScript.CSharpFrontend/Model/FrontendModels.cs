using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AvidScript.CSharpFrontend;

public sealed record FrontendSource(
    [property: JsonPropertyOrder(0)] string SourceId,
    [property: JsonPropertyOrder(1)] string Sha256,
    [property: JsonPropertyOrder(2)] int Length);

public sealed record FrontendSyntax(
    [property: JsonPropertyOrder(0)] string Kind,
    [property: JsonPropertyOrder(1)] FrontendSpan Span);

public sealed record FrontendToken(
    [property: JsonPropertyOrder(0)] string Kind,
    [property: JsonPropertyOrder(1)] string Text,
    [property: JsonPropertyOrder(2)] string ValueText,
    [property: JsonPropertyOrder(3)] FrontendSpan Span);

public sealed record FrontendTrivia(
    [property: JsonPropertyOrder(0)] string Kind,
    [property: JsonPropertyOrder(1)] string Text,
    [property: JsonPropertyOrder(2)] FrontendSpan Span);

public sealed record FrontendDiagnostic(
    [property: JsonPropertyOrder(0)] string Code,
    [property: JsonPropertyOrder(1)] string Severity,
    [property: JsonPropertyOrder(2)] string Message,
    [property: JsonPropertyOrder(3)] FrontendSpan Span);

public sealed record FrontendDocument(
    [property: JsonPropertyOrder(0)] int SchemaVersion,
    [property: JsonPropertyOrder(1)] string Language,
    [property: JsonPropertyOrder(2)] string FrontendVersion,
    [property: JsonPropertyOrder(3)] FrontendSource Source,
    [property: JsonPropertyOrder(4)] bool Succeeded,
    [property: JsonPropertyOrder(5)] FrontendSyntax Syntax,
    [property: JsonPropertyOrder(6)] IReadOnlyList<FrontendToken> Tokens,
    [property: JsonPropertyOrder(7)] IReadOnlyList<FrontendTrivia> Trivia,
    [property: JsonPropertyOrder(8)] IReadOnlyList<FrontendDiagnostic> Diagnostics);
