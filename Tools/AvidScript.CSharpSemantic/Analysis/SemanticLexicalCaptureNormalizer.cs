using System;
using System.Collections.Generic;
using System.Linq;
using Microsoft.CodeAnalysis;

namespace AvidScript.CSharpSemantic;

internal sealed record SemanticLexicalCaptureProjection(
    IReadOnlyList<SemanticSymbol> Symbols,
    IReadOnlyList<SemanticCallable> Callables,
    IReadOnlyList<SemanticMethodBody> Methods,
    IReadOnlyList<SemanticControlFlowGraph> Graphs,
    IReadOnlyList<SemanticAsyncMethod> AsyncMethods,
    IReadOnlyList<SemanticDiagnostic> Diagnostics)
{
    public SemanticClosureProjection Closures { get; init; } = new(
        Array.Empty<SemanticClosureEnvironment>(), Array.Empty<SemanticClosureBinding>(), Array.Empty<SemanticDiagnostic>());
}

// Direct local calls preserve shared cells through explicit ref parameters.
// Lambda captures are diagnosed until an owned escaping environment is available.
internal static class SemanticLexicalCaptureNormalizer
{
    public static SemanticLexicalCaptureProjection Normalize(
        SemanticCompilationContext context,
        IReadOnlyList<SemanticSymbol> symbols,
        IReadOnlyList<SemanticCallable> callables,
        IReadOnlyList<SemanticMethodBody> methods,
        IReadOnlyList<SemanticControlFlowGraph> graphs,
        IReadOnlyList<SemanticAsyncMethod> asyncMethods)
    {
        Dictionary<string, SemanticExecutableBody> lexical = SemanticExecutableBodyResolver.Resolve(context)
            .Where(body => SemanticExecutableBodyResolver.IsLexicalMethod(body.Method))
            .ToDictionary(body => SemanticSymbolProjector.GetSymbolId(body.Method), StringComparer.Ordinal);
        if (lexical.Count == 0 || context.Compilation.GetDiagnostics().Any(item => item.Severity == DiagnosticSeverity.Error))
        {
            return new(symbols, callables, methods, graphs, asyncMethods, Array.Empty<SemanticDiagnostic>());
        }

        Dictionary<string, SemanticSymbol> symbolsById = symbols.ToDictionary(item => item.Id, StringComparer.Ordinal);
        // Accessor value parameters and omitted anonymous-method parameters have no
        // ParameterSyntax. They still have lexical ownership and can be captured.
        foreach (SemanticCallable callable in callables)
            foreach (SemanticCallableParameter parameter in callable.Parameters)
                if (!symbolsById.ContainsKey(parameter.SymbolId))
                    symbolsById.Add(parameter.SymbolId, new(parameter.SymbolId, "parameter", parameter.Name,
                        callable.MethodSymbolId, parameter.TypeId, $"{parameter.Name}:{parameter.TypeId}", false, "notapplicable",
                        symbolsById.GetValueOrDefault(callable.MethodSymbolId)?.Span ?? SemanticSpanFactory.Empty));
        Dictionary<string, SemanticMethodBody> bodies = methods.ToDictionary(item => item.MethodSymbolId, StringComparer.Ordinal);
        Dictionary<string, SemanticLexicalCapture> captures = new(StringComparer.Ordinal);
        Dictionary<string, SortedSet<string>> required = lexical.Keys.ToDictionary(
            id => id, _ => new SortedSet<string>(StringComparer.Ordinal), StringComparer.Ordinal);
        Dictionary<string, string[]> calls = new(StringComparer.Ordinal);
        foreach ((string id, SemanticExecutableBody body) in lexical)
        {
            SemanticOperation[] operations = Enumerate(bodies[id].Root).ToArray();
            calls[id] = operations.Where(op => (op.Kind is "invocation" or "method_reference") && op.SymbolId is not null
                    && lexical.ContainsKey(op.SymbolId))
                .Select(op => op.SymbolId!).Distinct(StringComparer.Ordinal).ToArray();
            foreach (SemanticOperation operation in operations)
            {
                if (operation.Kind is "local_reference" or "parameter_reference"
                    && operation.Constant is null && operation.SymbolId is { } symbolId
                    && symbolsById.TryGetValue(symbolId, out SemanticSymbol? symbol)
                    && symbol.ContainingSymbolId is { } owner && owner != id && symbol.TypeId is { } typeId)
                {
                    captures.TryAdd(symbolId, new(symbolId, owner, symbol.Name, typeId,
                        operation.Kind, symbol.Span));
                    required[id].Add(symbolId);
                }
                else if (operation.Kind == "instance_reference")
                {
                    IMethodSymbol receiverOwner = body.Method;
                    while (SemanticExecutableBodyResolver.IsLexicalMethod(receiverOwner))
                        receiverOwner = (IMethodSymbol)receiverOwner.ContainingSymbol;
                    string ownerId = SemanticSymbolProjector.GetSymbolId(receiverOwner);
                    string captureId = "receiver:" + ownerId;
                    captures.TryAdd(captureId, new(captureId, ownerId, "this",
                        operation.TypeId!, "instance_reference", operation.Span, true));
                    required[id].Add(captureId);
                }
            }
        }

        bool changed;
        do
        {
            changed = false;
            foreach (string id in lexical.Keys.OrderBy(id => id, StringComparer.Ordinal))
                foreach (string target in calls[id])
                    foreach (string captureId in required[target].ToArray())
                        if (captures[captureId].Owner != id)
                            changed |= required[id].Add(captureId);
        } while (changed);

        SemanticClosureProjection closures = SemanticClosurePlanner.Project(context, captures, required, methods);
        Dictionary<string, Dictionary<string, SemanticCallableParameter>> parameters = new(StringComparer.Ordinal);
        List<SemanticSymbol> resultSymbols = symbolsById.Values.Select(symbol => lexical.ContainsKey(symbol.Id)
            ? symbol with { IsStatic = true } : symbol).ToList();
        SemanticCallable[] resultCallables = callables.Select(callable =>
        {
            if (!required.TryGetValue(callable.MethodSymbolId, out SortedSet<string>? ids)) return callable;
            Dictionary<string, SemanticCallableParameter> environment = new(StringComparer.Ordinal);
            foreach (string id in ids)
            {
                SemanticLexicalCapture capture = captures[id];
                int ordinal = callable.Parameters.Count + environment.Count;
                string parameterId = $"symbol:parameter:{callable.MethodSymbolId}:capture:{id}";
                SemanticCallableParameter parameter = new(ordinal, parameterId, capture.Name,
                    capture.TypeId, capture.IsReceiver ? "none" : "ref");
                environment.Add(id, parameter);
                resultSymbols.Add(new(parameterId, "parameter", capture.Name, callable.MethodSymbolId,
                    capture.TypeId, $"{capture.Name}:{capture.TypeId}", false, "notapplicable", capture.Span));
            }
            parameters.Add(callable.MethodSymbolId, environment);
            return callable with { IsStatic = true, Parameters = callable.Parameters.Concat(environment.Values).ToArray() };
        }).ToArray();

        SemanticOperation Rewrite(SemanticOperation operation, string ownerId)
        {
            if (operation.Kind == "local_reference" && operation.Constant is not null)
                return operation with { Kind = "literal", SymbolId = null, Children = Array.Empty<SemanticOperation>() };
            SemanticOperation[] children = operation.Children.Select(child => Rewrite(child, ownerId)).ToArray();
            SemanticOperation rewritten = operation with { Children = children };
            string? captureId = operation.Kind == "instance_reference"
                ? required.GetValueOrDefault(ownerId)?.FirstOrDefault(id => captures[id].IsReceiver)
                : operation.Kind is "local_reference" or "parameter_reference" ? operation.SymbolId : null;
            if (captureId is not null && parameters.TryGetValue(ownerId, out var environment)
                && environment.TryGetValue(captureId, out SemanticCallableParameter? parameter))
            {
                rewritten = rewritten with { Kind = "parameter_reference", SymbolId = parameter.SymbolId };
            }
            if (operation.Kind == "invocation" && operation.SymbolId is { } target
                && parameters.TryGetValue(target, out var targetEnvironment))
            {
                List<SemanticOperation> arguments = children.ToList();
                foreach ((string id, SemanticCallableParameter targetParameter) in targetEnvironment)
                {
                    SemanticLexicalCapture capture = captures[id];
                    SemanticOperation value = new(capture.Kind, true, null, false, false, false, false,
                        capture.TypeId, capture.IsReceiver ? null : capture.Id, Array.Empty<string>(),
                        null, null, null, null, null, operation.Span, Array.Empty<SemanticOperation>());
                    value = Rewrite(value, ownerId);
                    arguments.Add(new("argument", true, null, false, false, false, false,
                        capture.TypeId, targetParameter.SymbolId, Array.Empty<string>(), null,
                        null, null, null, null, operation.Span, new[] { value }));
                }
                rewritten = rewritten with { Children = arguments.ToArray() };
            }
            return rewritten;
        }

        SemanticMethodBody[] resultMethods = methods.Select(body => body with
            { Root = Rewrite(body.Root, body.MethodSymbolId) }).ToArray();
        SemanticControlFlowGraph[] resultGraphs = graphs.Select(graph => graph with
        {
            Blocks = graph.Blocks.Select(block => block with
            {
                Operations = block.Operations.Select(op => Rewrite(op, graph.MethodSymbolId)).ToArray(),
                BranchValue = block.BranchValue is null ? null : Rewrite(block.BranchValue, graph.MethodSymbolId),
            }).ToArray(),
        }).ToArray();
        List<SemanticDiagnostic> diagnostics = new(closures.Diagnostics);
        foreach ((string id, SemanticExecutableBody body) in lexical)
            if (body.Method.MethodKind == MethodKind.AnonymousFunction && required[id].Count != 0)
                diagnostics.Add(new(SemanticLambdaPolicy.DiagnosticCode, "error",
                    "This lambda captures activation state and requires a managed closure environment, which is not yet implemented.",
                    SemanticSpanFactory.Create(body.Unit.SourceText, body.Declaration.Span)));
        SemanticAsyncMethod[] resultAsync = asyncMethods.Select(method =>
        {
            SemanticAsyncSegment[] segments = method.Segments.Select(segment => segment with
            {
                Statements = segment.Statements.Select(statement => statement with
                    { Operation = Rewrite(statement.Operation, method.MethodSymbolId) }).ToArray(),
                Transfer = segment.Transfer is not { Condition: { } condition } transfer
                    ? segment.Transfer : transfer with { Condition = Rewrite(condition, method.MethodSymbolId) },
                AwaitSite = segment.AwaitSite is not { } site ? null : site with
                {
                    Arguments = site.Arguments.Select(op => Rewrite(op, method.MethodSymbolId)).ToArray(),
                    CancellationToken = site.CancellationToken is null ? null
                        : Rewrite(site.CancellationToken, method.MethodSymbolId),
                },
            }).ToArray();
            SemanticAsyncProjector.TryAttachStateFrames(segments, diagnostics,
                method.Lowering == SemanticAsyncMethod.ContinuationCfgLowering, out var framed);
            return method with { Segments = framed };
        }).ToArray();
        return new(resultSymbols.OrderBy(symbol => symbol.Id, StringComparer.Ordinal).ToArray(),
            resultCallables, resultMethods, resultGraphs, resultAsync, diagnostics) { Closures = closures };
    }

    private static IEnumerable<SemanticOperation> Enumerate(SemanticOperation operation)
    {
        yield return operation;
        foreach (SemanticOperation child in operation.Children)
            foreach (SemanticOperation descendant in Enumerate(child)) yield return descendant;
    }
}
