using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using Microsoft.CodeAnalysis;

namespace AvidScript.CSharpSemantic;

public static class SemanticAnalyzer
{
    private const int CurrentSchemaVersion = 2;
    private const string CurrentSemanticVersion = "1.1";

    public static SemanticDocument Analyze(string source, string sourceId, string frontendSourceSha256)
    {
        ArgumentNullException.ThrowIfNull(source);
        ArgumentException.ThrowIfNullOrWhiteSpace(sourceId);
        ArgumentException.ThrowIfNullOrWhiteSpace(frontendSourceSha256);

        string sourceSha256 = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(source))).ToLowerInvariant();
        SemanticSource semanticSource = new(sourceId, sourceSha256, frontendSourceSha256, source.Length);
        if (!sourceSha256.Equals(frontendSourceSha256, StringComparison.OrdinalIgnoreCase))
        {
            return new SemanticDocument(
                CurrentSchemaVersion,
                "csharp",
                CurrentSemanticVersion,
                semanticSource,
                false,
                Array.Empty<SemanticType>(),
                Array.Empty<SemanticSymbol>(),
                Array.Empty<SemanticMethodBody>(),
                new[]
                {
                    new SemanticDiagnostic(
                        "ASCS1001",
                        "error",
                        "The current source hash does not match the frontend artifact source hash.",
                        SemanticSpanFactory.Empty),
                });
        }

        SemanticCompilationContext context = SemanticCompilationFactory.Create(source, sourceId);
        SemanticTypeRegistry typeRegistry = new();
        IReadOnlyList<SemanticSymbol> symbols = SemanticSymbolProjector.Project(context, typeRegistry);
        SemanticOperationProjection operationProjection = SemanticOperationProjector.Project(context, typeRegistry);
        IReadOnlyList<SemanticDiagnostic> compilerDiagnostics = context.Compilation
            .GetDiagnostics()
            .Where(diagnostic => diagnostic.Location == Location.None || diagnostic.Location.SourceTree == context.SyntaxTree)
            .Select(diagnostic => ProjectDiagnostic(diagnostic, context))
            .OrderBy(diagnostic => diagnostic.Span.Start)
            .ThenBy(diagnostic => diagnostic.Code, StringComparer.Ordinal)
            .ToArray();
        IReadOnlyList<SemanticDiagnostic> diagnostics = compilerDiagnostics
            .Concat(operationProjection.Diagnostics)
            .OrderBy(diagnostic => diagnostic.Span.Start)
            .ThenBy(diagnostic => diagnostic.Code, StringComparer.Ordinal)
            .ToArray();
        bool succeeded = diagnostics.All(diagnostic => diagnostic.Severity != "error");

        return new SemanticDocument(
            CurrentSchemaVersion,
            "csharp",
            CurrentSemanticVersion,
            semanticSource,
            succeeded,
            typeRegistry.Build(),
            symbols,
            operationProjection.Methods,
            diagnostics);
    }

    private static SemanticDiagnostic ProjectDiagnostic(
        Diagnostic diagnostic,
        SemanticCompilationContext context)
    {
        SemanticSpan span = diagnostic.Location.IsInSource
            ? SemanticSpanFactory.Create(context.SourceText, diagnostic.Location.SourceSpan)
            : SemanticSpanFactory.Empty;
        return new SemanticDiagnostic(
            diagnostic.Id,
            diagnostic.Severity.ToString().ToLowerInvariant(),
            diagnostic.GetMessage(CultureInfo.InvariantCulture),
            span);
    }
}
