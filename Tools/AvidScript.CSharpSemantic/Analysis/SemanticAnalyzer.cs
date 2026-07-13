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
    private const int CurrentSchemaVersion = 3;
    private const string CurrentSemanticVersion = "1.3";

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
                Array.Empty<SemanticControlFlowGraph>(),
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
        SemanticSupportProjection supportProjection = SemanticSupportPolicy.ProjectDocument(context);
        SemanticOperationProjection operationProjection = SemanticOperationProjector.Project(context, typeRegistry);
        SemanticControlFlowProjection controlFlowProjection = SemanticControlFlowProjector.Project(context, typeRegistry);
        IReadOnlyList<SemanticDiagnostic> supportDiagnostics = supportProjection.Diagnostics
            .Concat(operationProjection.Diagnostics)
            .GroupBy(diagnostic =>
                (diagnostic.Code, diagnostic.Severity, diagnostic.Span.Start, diagnostic.Span.Length))
            .Select(group => group.First())
            .ToArray();
        bool hasSupportErrors = supportDiagnostics.Any(diagnostic => diagnostic.Severity == "error");
        IReadOnlyList<SemanticControlFlowGraph> controlFlowGraphs = hasSupportErrors
            ? Array.Empty<SemanticControlFlowGraph>()
            : controlFlowProjection.Graphs;
        IReadOnlyList<SemanticDiagnostic> compilerDiagnostics = context.Compilation
            .GetDiagnostics()
            .Where(diagnostic => diagnostic.Location == Location.None || diagnostic.Location.SourceTree == context.SyntaxTree)
            .Select(diagnostic => ProjectDiagnostic(diagnostic, context))
            .OrderBy(diagnostic => diagnostic.Span.Start)
            .ThenBy(diagnostic => diagnostic.Code, StringComparer.Ordinal)
            .ToArray();
        IReadOnlyList<SemanticDiagnostic> diagnostics = compilerDiagnostics
            .Concat(supportDiagnostics)
            .Concat(controlFlowProjection.Diagnostics)
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
            controlFlowGraphs,
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
