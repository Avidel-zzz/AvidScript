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
    public static SemanticDocument Analyze(string source, string sourceId, string frontendSourceSha256)
    {
        return Analyze(source, sourceId, frontendSourceSha256, Array.Empty<SemanticReferenceSource>());
    }

    public static SemanticDocument Analyze(
        string source,
        string sourceId,
        string frontendSourceSha256,
        IReadOnlyList<SemanticReferenceSource> referenceSources)
    {
        ArgumentNullException.ThrowIfNull(source);
        ArgumentException.ThrowIfNullOrWhiteSpace(sourceId);
        ArgumentException.ThrowIfNullOrWhiteSpace(frontendSourceSha256);
        ArgumentNullException.ThrowIfNull(referenceSources);

        string sourceSha256 = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(source))).ToLowerInvariant();
        SemanticSource semanticSource = new(sourceId, sourceSha256, frontendSourceSha256, source.Length);
        if (!sourceSha256.Equals(frontendSourceSha256, StringComparison.OrdinalIgnoreCase))
        {
            return new SemanticDocument(
                SemanticContract.CurrentSchemaVersion,
                "csharp",
                SemanticContract.CurrentSemanticVersion,
                semanticSource,
                false,
                Array.Empty<SemanticType>(),
                Array.Empty<SemanticTypeShape>(),
                Array.Empty<SemanticSymbol>(),
                Array.Empty<SemanticCallable>(),
                Array.Empty<SemanticMethodBody>(),
                Array.Empty<SemanticControlFlowGraph>(),
                new SemanticReachability(
                    "failed",
                    Array.Empty<string>(),
                    Array.Empty<string>(),
                    Array.Empty<SemanticReachableImport>()),
                new[]
                {
                    new SemanticDiagnostic(
                        "ASCS1001",
                        "error",
                        "The current source hash does not match the frontend artifact source hash.",
                        SemanticSpanFactory.Empty),
                });
        }

        SemanticCompilationContext context = SemanticCompilationFactory.Create(source, sourceId, referenceSources);
        SemanticTypeRegistry typeRegistry = new();
        IReadOnlyList<SemanticSymbol> symbols = SemanticSymbolProjector.Project(context, typeRegistry);
        SemanticStateContractProjection stateContractProjection = SemanticStateContractProjector.Project(
            context,
            typeRegistry);
        SemanticCallableProjection callableProjection = SemanticCallableProjector.Project(context, typeRegistry);
        SemanticGameplayEventProjection gameplayEventProjection =
            SemanticGameplayEventProjector.Project(context, callableProjection.Callables);
        SemanticDelegateEventProjection delegateEventProjection =
            SemanticDelegateEventProjector.Project(context);
        SemanticSupportProjection supportProjection = SemanticSupportPolicy.ProjectDocument(context);
        SemanticOperationProjection operationProjection = SemanticOperationProjector.Project(context, typeRegistry);
        SemanticControlFlowProjection controlFlowProjection = SemanticControlFlowProjector.Project(context, typeRegistry);
        IReadOnlyList<SemanticDiagnostic> supportDiagnostics = supportProjection.Diagnostics
            .Concat(operationProjection.Diagnostics)
            .Concat(callableProjection.Diagnostics)
            .Concat(stateContractProjection.Diagnostics)
            .Concat(gameplayEventProjection.Diagnostics)
            .Concat(delegateEventProjection.Diagnostics)
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
        SemanticReachability reachability = SemanticReachabilityProjector.Project(
            callableProjection.Callables,
            controlFlowGraphs,
            gameplayEventProjection.Callbacks,
            delegateEventProjection.Callbacks);

        return new SemanticDocument(
            SemanticContract.CurrentSchemaVersion,
            "csharp",
            SemanticContract.CurrentSemanticVersion,
            semanticSource,
            succeeded,
            typeRegistry.Build(),
            typeRegistry.BuildShapes(),
            symbols,
            callableProjection.Callables,
            operationProjection.Methods,
            controlFlowGraphs,
            reachability,
            diagnostics)
        {
            StateContracts = stateContractProjection.Contracts,
            GameplayEventCallbacks = gameplayEventProjection.Callbacks,
            DelegateEventCallbacks = delegateEventProjection.Callbacks,
        };
    }

    private static SemanticDiagnostic ProjectDiagnostic(
        Diagnostic diagnostic,
        SemanticCompilationContext context)
    {
        bool isPrimarySource = diagnostic.Location.IsInSource
            && diagnostic.Location.SourceTree == context.SyntaxTree;
        SemanticSpan span = isPrimarySource
            ? SemanticSpanFactory.Create(context.SourceText, diagnostic.Location.SourceSpan)
            : SemanticSpanFactory.Empty;
        return new SemanticDiagnostic(
            diagnostic.Id,
            diagnostic.Severity.ToString().ToLowerInvariant(),
            diagnostic.GetMessage(CultureInfo.InvariantCulture),
            span);
    }
}
