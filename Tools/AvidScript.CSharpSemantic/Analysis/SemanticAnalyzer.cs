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
        return Analyze(
            source,
            sourceId,
            frontendSourceSha256,
            referenceSources,
            new SemanticCompilerWorkspace());
    }

    public static SemanticDocument Analyze(
        string source,
        string sourceId,
        string frontendSourceSha256,
        IReadOnlyList<SemanticReferenceSource> referenceSources,
        SemanticCompilerWorkspace workspace)
    {
        ArgumentNullException.ThrowIfNull(source);
        ArgumentException.ThrowIfNullOrWhiteSpace(sourceId);
        ArgumentException.ThrowIfNullOrWhiteSpace(frontendSourceSha256);
        ArgumentNullException.ThrowIfNull(referenceSources);
        ArgumentNullException.ThrowIfNull(workspace);

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

        SemanticCompilationContext context = SemanticCompilationFactory.Create(
            source,
            sourceId,
            referenceSources,
            workspace);
        SemanticTypeRegistry typeRegistry = new();
        IReadOnlyList<SemanticSymbol> symbols = SemanticSymbolProjector.Project(context, typeRegistry);
        SemanticStateContractProjection stateContractProjection = SemanticStateContractProjector.Project(
            context,
            typeRegistry);
        SemanticUeTypeProjection ueTypeProjection = SemanticUeTypeProjector.Project(context, typeRegistry);
        SemanticCallableProjection callableProjection = SemanticCallableProjector.Project(context, typeRegistry);
        SemanticGameplayEventProjection gameplayEventProjection =
            SemanticGameplayEventProjector.Project(context, callableProjection.Callables);
        SemanticDelegateEventProjection delegateEventProjection =
            SemanticDelegateEventProjector.Project(context, typeRegistry);
        SemanticContinuationProjection continuationProjection =
            SemanticContinuationProjector.Project(context);
        SemanticSupportProjection supportProjection = SemanticSupportPolicy.ProjectDocument(context);
        SemanticOperationProjection operationProjection = SemanticOperationProjector.Project(context, typeRegistry);
        SemanticAsyncProjection asyncProjection = SemanticAsyncProjector.Project(
            context,
            typeRegistry,
            callableProjection.Callables);
        SemanticControlFlowProjection controlFlowProjection = SemanticControlFlowProjector.Project(
            context,
            typeRegistry,
            asyncProjection.ControlledMethodSymbolIds);
        symbols = symbols.Concat(controlFlowProjection.CompilerLocalSymbols)
            .OrderBy(symbol => symbol.Id, StringComparer.Ordinal)
            .ToArray();
        SemanticLexicalCaptureProjection lexicalCaptures = SemanticLexicalCaptureNormalizer.Normalize(
            context, symbols, callableProjection.Callables, operationProjection.Methods,
            controlFlowProjection.Graphs, asyncProjection.Methods);
        symbols = lexicalCaptures.Symbols;
        callableProjection = callableProjection with { Callables = lexicalCaptures.Callables };
        operationProjection = operationProjection with { Methods = lexicalCaptures.Methods };
        controlFlowProjection = controlFlowProjection with { Graphs = lexicalCaptures.Graphs };
        asyncProjection = asyncProjection with { Methods = lexicalCaptures.AsyncMethods };
        SemanticReachability sourceReachability = SemanticReachabilityProjector.Project(
            callableProjection.Callables, controlFlowProjection.Graphs,
            gameplayEventProjection.Callbacks, delegateEventProjection.Callbacks,
            continuationProjection.Callbacks, ueTypeProjection.Declarations, asyncProjection.Methods);
        SemanticGenericProjection genericProjection = SemanticGenericSpecializer.Project(
            typeRegistry.Build(), typeRegistry.BuildShapes(), symbols, callableProjection.Callables,
            operationProjection.Methods, controlFlowProjection.Graphs, asyncProjection.Methods,
            sourceReachability.ReachableCallableIds.ToHashSet(StringComparer.Ordinal));
        symbols = genericProjection.Symbols;
        callableProjection = callableProjection with { Callables = genericProjection.Callables };
        operationProjection = operationProjection with { Methods = genericProjection.Methods };
        controlFlowProjection = controlFlowProjection with { Graphs = genericProjection.Graphs };
        asyncProjection = asyncProjection with { Methods = genericProjection.AsyncMethods };
        IReadOnlyList<SemanticDiagnostic> asyncLanguageErrorDiagnostics = asyncProjection.Methods
            .SelectMany(method => method.ErrorPlan?.Throws ?? Array.Empty<SemanticAsyncThrowSite>())
            .Select(site => new SemanticDiagnostic(
                "ASCS5422", "error",
                "Async Task<int> throw has a semantic plan, but executable task-fault lowering is not available yet.",
                site.Span))
            .ToArray();
        IReadOnlyList<SemanticDiagnostic> supportDiagnostics = supportProjection.Diagnostics
            .Concat(lexicalCaptures.Diagnostics)
            .Concat(genericProjection.Diagnostics)
            .Concat(operationProjection.Diagnostics)
            .Concat(asyncProjection.Diagnostics)
            .Concat(asyncLanguageErrorDiagnostics)
            .Concat(callableProjection.Diagnostics)
            .Concat(stateContractProjection.Diagnostics)
            .Concat(ueTypeProjection.Diagnostics)
            .Concat(gameplayEventProjection.Diagnostics)
            .Concat(delegateEventProjection.Diagnostics)
            .Concat(continuationProjection.Diagnostics)
            .GroupBy(diagnostic =>
                (diagnostic.Code, diagnostic.Severity, diagnostic.Span.Start, diagnostic.Span.Length))
            .Select(group => group.First())
            .ToArray();
        // ASCS5422 marks a bounded async throw plan for opt-in lowering. It does not
        // invalidate control-flow graphs for unrelated synchronous methods.
        bool hasBlockingSupportErrors = supportDiagnostics.Any(diagnostic =>
            diagnostic.Severity == "error" && diagnostic.Code != "ASCS5422");
        IReadOnlyList<SemanticControlFlowGraph> controlFlowGraphs =
            hasBlockingSupportErrors && controlFlowProjection.ExceptionFlows.Count == 0
            ? Array.Empty<SemanticControlFlowGraph>()
            : controlFlowProjection.Graphs;
        IReadOnlyList<SemanticDiagnostic> compilerDiagnostics = context.Compilation
            .GetDiagnostics()
            .Select(diagnostic => ProjectDiagnostic(diagnostic, context))
            .OrderBy(diagnostic => diagnostic.Span.Start)
            .ThenBy(diagnostic => diagnostic.Span.Length)
            .ThenBy(diagnostic => diagnostic.Code, StringComparer.Ordinal)
            .ThenBy(diagnostic => diagnostic.Severity, StringComparer.Ordinal)
            .ThenBy(diagnostic => diagnostic.Message, StringComparer.Ordinal)
            .ToArray();
        IReadOnlyList<SemanticDiagnostic> diagnostics = compilerDiagnostics
            .Concat(supportDiagnostics)
            .Concat(controlFlowProjection.Diagnostics)
            .OrderBy(diagnostic => diagnostic.Span.Start)
            .ThenBy(diagnostic => diagnostic.Span.Length)
            .ThenBy(diagnostic => diagnostic.Code, StringComparer.Ordinal)
            .ThenBy(diagnostic => diagnostic.Severity, StringComparer.Ordinal)
            .ThenBy(diagnostic => diagnostic.Message, StringComparer.Ordinal)
            .ToArray();
        bool succeeded = diagnostics.All(diagnostic => diagnostic.Severity != "error");
        SemanticReachability reachability = SemanticReachabilityProjector.Project(
            callableProjection.Callables,
            controlFlowGraphs,
            gameplayEventProjection.Callbacks,
            delegateEventProjection.Callbacks,
            continuationProjection.Callbacks,
            ueTypeProjection.Declarations,
            asyncProjection.Methods);

        bool hasExceptionFlows = controlFlowProjection.ExceptionFlows.Count > 0;
        bool exceptionFlowOnlyFailure = hasExceptionFlows && diagnostics.All(diagnostic =>
            diagnostic.Severity != "error" || diagnostic.Code == "ASCS3001");
        SemanticUeMethodCatalog methodCatalog = succeeded || exceptionFlowOnlyFailure
            ? SemanticUeMethodCatalogProjector.Project(context, typeRegistry, ueTypeProjection.Declarations, callableProjection.Callables)
            : SemanticUeMethodCatalog.Empty;
        bool hasTaskResults = asyncProjection.Methods.Any(method => method.TaskResultTypeId is not null
            || method.Segments.Any(segment => segment.AwaitSite?.TaskCallableId is not null));
        bool hasTaskLocals = asyncProjection.Methods.Any(method => method.Segments.Any(segment =>
            segment.AwaitSite?.TaskLocalSymbolId is not null));
        bool hasTaskAssignments = asyncProjection.Methods.Any(method => method.Segments.Any(segment =>
            segment.AwaitSite?.ResultStorageKind == "static_field"));
        bool hasTaskExistingLocalAssignments = asyncProjection.Methods.Any(method => method.Segments.Any(segment =>
            segment.AwaitSite?.ResultStorageKind == "existing_local"));
        bool hasTaskAliases = asyncProjection.Methods.Any(method => method.TaskLocalSymbolIds is not null);
        bool hasAsyncLanguageErrors = asyncProjection.Methods.Any(method => method.ErrorPlan is not null);
        bool hasTaskLanguageErrors = hasExceptionFlows && hasTaskResults;
        return new SemanticDocument(
            hasAsyncLanguageErrors ? SemanticContract.AsyncLanguageErrorSchemaVersion
                : hasTaskLanguageErrors ? SemanticContract.TaskLanguageErrorSchemaVersion
                : hasExceptionFlows ? SemanticContract.ExceptionFlowSchemaVersion
                : hasTaskAliases ? SemanticContract.TaskAliasSchemaVersion
                : hasTaskExistingLocalAssignments ? SemanticContract.TaskExistingLocalSchemaVersion
                : hasTaskAssignments ? SemanticContract.TaskAssignmentSchemaVersion
                : hasTaskLocals ? SemanticContract.TaskLocalSchemaVersion
                : hasTaskResults ? SemanticContract.TaskResultSchemaVersion
                : SemanticContract.CurrentSchemaVersion,
            "csharp",
            hasAsyncLanguageErrors ? SemanticContract.AsyncLanguageErrorSemanticVersion
                : hasTaskLanguageErrors ? SemanticContract.TaskLanguageErrorSemanticVersion
                : hasExceptionFlows ? SemanticContract.ExceptionFlowSemanticVersion
                : hasTaskAliases ? SemanticContract.TaskAliasSemanticVersion
                : hasTaskExistingLocalAssignments ? SemanticContract.TaskExistingLocalSemanticVersion
                : hasTaskAssignments ? SemanticContract.TaskAssignmentSemanticVersion
                : hasTaskLocals ? SemanticContract.TaskLocalSemanticVersion
                : hasTaskResults ? SemanticContract.TaskResultSemanticVersion
                : SemanticContract.CurrentSemanticVersion,
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
            EventSubscriptions = delegateEventProjection.Subscriptions,
            ContinuationCallbacks = continuationProjection.Callbacks,
            AsyncMethods = hasExceptionFlows && !hasTaskLanguageErrors
                ? Array.Empty<SemanticAsyncMethod>() : asyncProjection.Methods,
            UeTypeDeclarations = ueTypeProjection.Declarations,
            DelegateTypes = typeRegistry.BuildDelegateTypes(),
            ClassTypes = typeRegistry.BuildClassTypes(),
            ClosureEnvironments = lexicalCaptures.Closures.Environments,
            ClosureBindings = lexicalCaptures.Closures.Bindings,
            UeMethodCatalog = methodCatalog,
            ExceptionFlows = hasExceptionFlows ? controlFlowProjection.ExceptionFlows : null,
            RejectedAsyncExceptionFlows = controlFlowProjection.RejectedAsyncExceptionFlows.Count > 0
                ? controlFlowProjection.RejectedAsyncExceptionFlows : null,
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
