using System;
using System.Collections.Generic;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.FlowAnalysis;
using Microsoft.CodeAnalysis.Operations;

namespace AvidScript.CSharpSemantic;

internal sealed record SemanticControlFlowProjection(
    IReadOnlyList<SemanticControlFlowGraph> Graphs,
    IReadOnlyList<SemanticDiagnostic> Diagnostics,
    IReadOnlyList<SemanticSymbol> CompilerLocalSymbols,
    IReadOnlyList<SemanticExceptionFlow> ExceptionFlows,
    IReadOnlyList<SemanticExceptionFlow> RejectedAsyncExceptionFlows,
    IReadOnlyList<SemanticAsyncMethod> AsyncExceptionMethods);

internal static class SemanticControlFlowProjector
{
    public static SemanticControlFlowProjection Project(
        SemanticCompilationContext context,
        SemanticTypeRegistry typeRegistry,
        IReadOnlySet<string> controlledAsyncMethodIds,
        IReadOnlyList<SemanticAsyncMethod> controlledAsyncMethods,
        IReadOnlyList<SemanticCallable> callables,
        bool enableAsyncExceptionFlow = false,
        bool enableDirectAwaitCleanup = false,
        bool enableAsyncCancellationFlow = false)
    {
        List<SemanticDiagnostic> diagnostics = new();
        List<SemanticControlFlowGraph> graphs = new();
        List<SemanticSymbol> compilerLocalSymbols = new();
        List<SemanticExceptionFlow> exceptionFlows = new();
        List<SemanticExceptionFlow> rejectedAsyncExceptionFlows = new();
        List<SemanticAsyncMethod> asyncExceptionMethods = new();
        int nextAsyncCallbackId = Math.Max(SemanticContinuationProjector.CompilerCallbackIdStart,
            controlledAsyncMethods.SelectMany(method => method.Segments)
                .Select(segment => segment.AwaitSite?.CallbackId ?? -1)
                .DefaultIfEmpty(-1).Max() + 1);
        Diagnostic? compilerError = context.Compilation.GetDiagnostics()
            .Where(diagnostic => diagnostic.Severity == DiagnosticSeverity.Error)
            .OrderBy(diagnostic => diagnostic.Location.IsInSource
                ? diagnostic.Location.SourceSpan.Start
                : int.MaxValue)
            .ThenBy(diagnostic => diagnostic.Id, StringComparer.Ordinal)
            .FirstOrDefault();
        if (compilerError is not null)
        {
            diagnostics.Add(CreateDiagnostic(
                "ASCS3004",
                "Control-flow projection requires a valid C# compilation.",
                compilerError.Location.IsInSource && compilerError.Location.SourceTree == context.SyntaxTree
                    ? SemanticSpanFactory.Create(context.SourceText, compilerError.Location.SourceSpan)
                    : SemanticSpanFactory.Empty));
            return new SemanticControlFlowProjection(Array.Empty<SemanticControlFlowGraph>(), diagnostics,
                Array.Empty<SemanticSymbol>(), Array.Empty<SemanticExceptionFlow>(),
                Array.Empty<SemanticExceptionFlow>(), Array.Empty<SemanticAsyncMethod>());
        }

        foreach (SemanticExecutableBody body in SemanticExecutableBodyResolver.Resolve(context))
        {
            SemanticModel semanticModel = context.Compilation.GetSemanticModel(
                body.Unit.SyntaxTree,
                ignoreAccessibility: false);
            SemanticSpan bodySpan = SemanticSpanFactory.Create(
                body.Unit.SourceText,
                body.Declaration.Span);
            if (body.Method.IsAsync)
            {
                if (controlledAsyncMethodIds.Contains(
                    SemanticSymbolProjector.GetSymbolId(body.Method)))
                {
                    continue;
                }

                // Keep Roslyn's source-backed exception regions on a rejected
                // async method. They are diagnostic evidence for the future
                // continuation CFG, never executable synchronous flow.
                if (SemanticExceptionFlowProjector.IsAsyncRegionCandidate(body))
                {
                    try
                    {
                        ControlFlowGraph asyncGraph = CreateGraph(body, semanticModel);
                        SemanticExceptionFlow? sourceFlow =
                            SemanticExceptionFlowProjector.Project(
                                body, semanticModel, asyncGraph, typeRegistry, diagnostics);
                        if (sourceFlow is not null)
                        {
                            if (body.Unit.SyntaxTree == context.SyntaxTree
                                && body.Declaration is MethodDeclarationSyntax { Body: { } asyncBody }
                                && SemanticAsyncProjector.TryGetSupportedTaskResult(
                                    context.Compilation, body.Method.ReturnType,
                                    out ITypeSymbol? resultType))
                            {
                                List<SemanticDiagnostic> previewDiagnostics = new();
                                int previewCallbackId = nextAsyncCallbackId;
                                if (SemanticAsyncControlFlowProjector.TryProject(
                                        context, semanticModel, asyncBody,
                                        sourceFlow.MethodSymbolId, typeRegistry,
                                        previewDiagnostics, ref previewCallbackId,
                                        out SemanticAsyncControlFlowProjection? preview,
                                        allowValueReturns: true, resultType: resultType,
                                        previewSuspendedFinally: true,
                                        allowDirectAwaitCleanup: enableDirectAwaitCleanup,
                                        allowAsyncCancellationFlow: enableAsyncCancellationFlow)
                                    && preview is not null
                                    && preview.Segments.Any(segment => segment.Transfer is
                                        { Kind: SemanticAsyncMethod.AwaitTransferKind,
                                            SecondaryTarget: >= 0 }
                                        || enableDirectAwaitCleanup && segment.Transfer is
                                            { Kind: SemanticAsyncMethod.AwaitTransferKind,
                                                CancellationTarget: >= 0 }
                                        || enableAsyncCancellationFlow && segment.Transfer?.Kind
                                            == SemanticAsyncMethod.RaiseExceptionTransferKind))
                                {
                                    SemanticAsyncStateSlot[] inputs = body.Method.Parameters
                                        .Select(parameter => new SemanticAsyncStateSlot(
                                            SemanticSymbolProjector.GetSymbolId(parameter),
                                            typeRegistry.Register(parameter.Type)))
                                        .Concat(body.Method.IsStatic
                                            ? Array.Empty<SemanticAsyncStateSlot>()
                                            : new[] { new SemanticAsyncStateSlot(
                                                SemanticAsyncMethod.ReceiverSymbol(sourceFlow.MethodSymbolId),
                                                typeRegistry.Register(body.Method.ContainingType)) })
                                        .OrderBy(slot => slot.SymbolId, StringComparer.Ordinal)
                                        .ToArray();
                                    if (SemanticAsyncProjector.TryAttachStateFrames(
                                            preview.Segments, previewDiagnostics,
                                            isControlFlow: true,
                                            out IReadOnlyList<SemanticAsyncSegment> framed,
                                            inputs)
                                        && SemanticAsyncExceptionRegionBinder.TryBind(
                                            context, (MethodDeclarationSyntax)body.Declaration,
                                            sourceFlow, preview,
                                            out IReadOnlyList<SemanticAsyncExceptionPreviewRegion>
                                                boundRegions,
                                            out IReadOnlyList<SemanticAsyncExceptionScope> boundScopes))
                                    {
                                        if (enableAsyncExceptionFlow
                                            && callables.Any(callable =>
                                                callable.MethodSymbolId == sourceFlow.MethodSymbolId
                                                && callable.HasBody && !callable.IsConstructor
                                                && callable.Export is null
                                                && callable.Import is null)
                                            && body.Method.Parameters.All(parameter =>
                                                parameter.RefKind == RefKind.None)
                                            && !body.Method.IsGenericMethod
                                            && !body.Method.ContainingType.IsGenericType
                                            && body.Method.ContainingType.TypeKind
                                                == TypeKind.Class)
                                        {
                                            SemanticAsyncMethod exceptionMethod = new(
                                                sourceFlow.MethodSymbolId, null,
                                                SemanticAsyncMethod.ContinuationCfgLowering,
                                                framed, bodySpan,
                                                preview.EntrySegmentOrdinal)
                                            {
                                                CompilerLocals = preview.CompilerLocals,
                                                InvocationInputs = inputs,
                                                LexicalScopes = preview.LexicalScopes,
                                                TaskResultTypeId = typeRegistry.Register(resultType!),
                                                TaskLocalSymbolIds = SemanticAsyncProjector.GetTaskAliasLocalIds(
                                                    context, semanticModel, asyncBody),
                                                TaskLocalLifetimes = SemanticAsyncTaskLocalProjector.ProjectLifetimes(
                                                    context, semanticModel, asyncBody, preview.LexicalScopes),
                                                ErrorPlan = preview.ErrorPlan,
                                                ExceptionPlan = new(
                                                    sourceFlow.SourceId,
                                                    sourceFlow.SourceLength,
                                                    boundRegions.Select(region =>
                                                        new SemanticAsyncExceptionRegion(
                                                            region.Kind,
                                                            region.RoslynRegionOrdinal,
                                                            region.SourceSpan,
                                                            region.Segments)).ToArray(),
                                                    sourceFlow.Catches)
                                                {
                                                    CancellationTypeId = enableAsyncCancellationFlow
                                                        ? typeRegistry.Register(context.Compilation.GetTypeByMetadataName(
                                                            "System.Threading.Tasks.TaskCanceledException")!) : null,
                                                    ExceptionScopes = enableAsyncCancellationFlow ? boundScopes : null,
                                                },
                                            };
                                            if (SemanticAsyncProjector.ValidateTaskLocalOwnership(
                                                    context, semanticModel, asyncBody, exceptionMethod, diagnostics))
                                            {
                                                asyncExceptionMethods.Add(exceptionMethod);
                                                nextAsyncCallbackId = previewCallbackId;
                                                continue;
                                            }
                                        }
                                        sourceFlow = sourceFlow with
                                        {
                                            AsyncContinuationPreview = new(
                                                framed,
                                                preview.EntrySegmentOrdinal,
                                                preview.CompilerLocals,
                                                preview.LexicalScopes,
                                                boundRegions),
                                        };
                                    }
                                }
                            }
                            rejectedAsyncExceptionFlows.Add(sourceFlow);
                        }
                    }
                    catch (ArgumentException)
                    {
                        diagnostics.Add(CreateInvalidGraphDiagnostic(bodySpan));
                    }
                    catch (InvalidOperationException)
                    {
                        diagnostics.Add(CreateInvalidGraphDiagnostic(bodySpan));
                    }
                }

                diagnostics.Add(CreateDiagnostic(
                    "ASCS3002",
                    "Async control flow is not supported by the current AvidScript semantic profile.",
                    bodySpan));
                continue;
            }

            if (body.Declaration.DescendantNodesAndSelf().OfType<YieldStatementSyntax>().Any())
            {
                diagnostics.Add(CreateDiagnostic(
                    "ASCS3003",
                    "Iterator control flow is not supported by the current AvidScript semantic profile.",
                    bodySpan));
                continue;
            }

            ControlFlowGraph graph;
            try
            {
                graph = CreateGraph(body, semanticModel);
            }
            catch (ArgumentException)
            {
                diagnostics.Add(CreateInvalidGraphDiagnostic(bodySpan));
                continue;
            }
            catch (InvalidOperationException)
            {
                diagnostics.Add(CreateInvalidGraphDiagnostic(bodySpan));
                continue;
            }

            if (HasUnsupportedExceptionFlow(graph))
            {
                bool hasExceptionSyntax = SemanticExceptionFlowProjector.IsCandidate(body);
                if (hasExceptionSyntax)
                {
                    SemanticExceptionFlow? exceptionFlow = SemanticExceptionFlowProjector.Project(
                        body, semanticModel, graph, typeRegistry, diagnostics);
                    if (exceptionFlow is not null) exceptionFlows.Add(exceptionFlow);
                }
                if (!hasExceptionSyntax
                    && SemanticArrayForEachControlFlowProjector.IsCandidate(body, semanticModel))
                {
                    if (!SemanticArrayForEachControlFlowProjector.TryProject(
                        context, body, semanticModel, typeRegistry, diagnostics,
                        out SemanticControlFlowGraph? arrayGraph,
                        out IReadOnlyList<SemanticSymbol> arrayCompilerLocals))
                    {
                        continue;
                    }
                    IReadOnlyList<string> unsupportedArrayKinds = GetUnsupportedOperationKinds(arrayGraph!);
                    if (unsupportedArrayKinds.Count > 0)
                    {
                        diagnostics.Add(CreateDiagnostic(
                            "ASCS3004",
                            $"Structured flow contains unsupported lowered operations: {string.Join(", ", unsupportedArrayKinds)}.",
                            bodySpan));
                        continue;
                    }
                    graphs.Add(arrayGraph!);
                    compilerLocalSymbols.AddRange(arrayCompilerLocals);
                    continue;
                }

                diagnostics.Add(CreateDiagnostic(
                    "ASCS3001",
                    "Exception control flow is not supported by the current AvidScript semantic profile.",
                    bodySpan));
                continue;
            }

            SemanticControlFlowGraph projected = ProjectGraph(
                graph,
                body.Method,
                body.Unit,
                typeRegistry);
            IReadOnlyList<string> unsupportedKinds = GetUnsupportedOperationKinds(projected);
            if (unsupportedKinds.Count > 0)
            {
                diagnostics.Add(CreateDiagnostic(
                    "ASCS3004",
                    $"The Roslyn control-flow graph contains unsupported lowered operations: {string.Join(", ", unsupportedKinds)}.",
                    bodySpan));
                continue;
            }

            graphs.Add(projected);
        }

        IReadOnlyList<SemanticDiagnostic> orderedDiagnostics = diagnostics
            .OrderBy(diagnostic => diagnostic.Span.Start)
            .ThenBy(diagnostic => diagnostic.Code, StringComparer.Ordinal)
            .ToArray();
        if (orderedDiagnostics.Count > 0)
        {
            bool hasExceptionFlows = exceptionFlows.Count > 0;
            return new SemanticControlFlowProjection(
                hasExceptionFlows
                    ? graphs.OrderBy(graph => graph.MethodSymbolId, StringComparer.Ordinal).ToArray()
                    : Array.Empty<SemanticControlFlowGraph>(),
                orderedDiagnostics,
                hasExceptionFlows
                    ? compilerLocalSymbols.OrderBy(symbol => symbol.Id, StringComparer.Ordinal).ToArray()
                    : Array.Empty<SemanticSymbol>(),
                exceptionFlows.OrderBy(flow => flow.MethodSymbolId, StringComparer.Ordinal).ToArray(),
                rejectedAsyncExceptionFlows.OrderBy(flow => flow.MethodSymbolId,
                    StringComparer.Ordinal).ToArray(),
                asyncExceptionMethods.OrderBy(method => method.Span.Start).ToArray());
        }

        return new SemanticControlFlowProjection(
            graphs.OrderBy(graph => graph.MethodSymbolId, StringComparer.Ordinal).ToArray(),
            orderedDiagnostics,
            compilerLocalSymbols.OrderBy(symbol => symbol.Id, StringComparer.Ordinal).ToArray(),
            Array.Empty<SemanticExceptionFlow>(),
            Array.Empty<SemanticExceptionFlow>(),
            asyncExceptionMethods.OrderBy(method => method.Span.Start).ToArray());
    }

    private static IReadOnlyList<string> GetUnsupportedOperationKinds(
        SemanticControlFlowGraph graph)
    {
        return graph.Blocks
            .SelectMany(block => block.BranchValue is null
                ? block.Operations
                : block.Operations.Append(block.BranchValue))
            .SelectMany(EnumerateOperation)
            .Where(operation => !operation.IsSupported)
            .Select(operation => operation.Kind)
            .Distinct(StringComparer.Ordinal)
            .OrderBy(kind => kind, StringComparer.Ordinal)
            .ToArray();
    }

    private static IEnumerable<SemanticOperation> EnumerateOperation(SemanticOperation operation)
    {
        yield return operation;
        foreach (SemanticOperation child in operation.Children)
        {
            foreach (SemanticOperation descendant in EnumerateOperation(child))
            {
                yield return descendant;
            }
        }
    }

    internal static ControlFlowGraph CreateGraph(
        SemanticExecutableBody body,
        SemanticModel semanticModel)
    {
        if (SemanticExecutableBodyResolver.IsLexicalMethod(body.Method))
        {
            SyntaxNode ownerDeclaration = body.Declaration.Ancestors().First(SemanticExecutableBodyResolver.IsExecutableDeclaration);
            IMethodSymbol ownerMethod = SemanticExecutableBodyResolver.GetMethodSymbol(ownerDeclaration, semanticModel)!;
            IOperation ownerOperation = SemanticExecutableBodyResolver.GetOperation(ownerDeclaration, semanticModel)!;
            while (!SemanticExecutableBodyResolver.IsLexicalMethod(ownerMethod)
                && ownerOperation.Parent is { } parent)
            {
                ownerOperation = parent;
            }
            ControlFlowGraph ownerGraph = CreateGraph(
                new SemanticExecutableBody(ownerDeclaration, ownerMethod, ownerOperation, body.Unit),
                semanticModel);
            if (body.Method.MethodKind == MethodKind.LocalFunction)
                return ownerGraph.GetLocalFunctionControlFlowGraph(body.Method);
            IFlowAnonymousFunctionOperation lambda = ownerGraph.Blocks
                .SelectMany(block => block.BranchValue is { } branch ? block.Operations.Append(branch) : block.Operations)
                .SelectMany(Descendants).OfType<IFlowAnonymousFunctionOperation>()
                .Single(operation => SymbolEqualityComparer.Default.Equals(operation.Symbol, body.Method));
            return ownerGraph.GetAnonymousFunctionControlFlowGraph(lambda);
        }
        return body.Operation switch
        {
            IMethodBodyOperation methodBody => ControlFlowGraph.Create(methodBody),
            IConstructorBodyOperation constructorBody => ControlFlowGraph.Create(constructorBody),
            IBlockOperation block when block.Parent is null => ControlFlowGraph.Create(block),
            _ => CreateSyntaxGraph(body.Declaration, semanticModel),
        };
    }

    private static IEnumerable<IOperation> Descendants(IOperation operation)
    {
        yield return operation;
        foreach (IOperation child in operation.ChildOperations)
            foreach (IOperation descendant in Descendants(child)) yield return descendant;
    }

    private static ControlFlowGraph CreateSyntaxGraph(
        SyntaxNode declaration,
        SemanticModel semanticModel)
    {
        SyntaxNode graphRoot = declaration switch
        {
            PropertyDeclarationSyntax property => property.ExpressionBody!.Expression,
            IndexerDeclarationSyntax indexer => indexer.ExpressionBody!.Expression,
            _ => declaration,
        };
        return ControlFlowGraph.Create(graphRoot, semanticModel) ??
            throw new InvalidOperationException("Roslyn did not create a control-flow graph.");
    }

    private static bool HasUnsupportedExceptionFlow(ControlFlowGraph graph)
    {
        bool hasUnsupportedRegion = EnumerateRegions(graph.Root).Any(region =>
            region.Kind is not ControlFlowRegionKind.Root and not ControlFlowRegionKind.LocalLifetime);
        bool hasUnsupportedBranch = graph.Blocks
            .SelectMany(GetAllBranches)
            .Any(branch => branch.Semantics is
                ControlFlowBranchSemantics.StructuredExceptionHandling or
                ControlFlowBranchSemantics.Throw or
                ControlFlowBranchSemantics.Rethrow or
                ControlFlowBranchSemantics.Error);
        return hasUnsupportedRegion || hasUnsupportedBranch;
    }

    private static IEnumerable<ControlFlowRegion> EnumerateRegions(ControlFlowRegion region)
    {
        yield return region;
        foreach (ControlFlowRegion nested in region.NestedRegions)
        {
            foreach (ControlFlowRegion descendant in EnumerateRegions(nested))
            {
                yield return descendant;
            }
        }
    }

    private static SemanticControlFlowGraph ProjectGraph(
        ControlFlowGraph graph,
        IMethodSymbol method,
        SemanticCompilationUnit unit,
        SemanticTypeRegistry typeRegistry)
    {
        SemanticCaptureRegistry captureRegistry = new();
        IReadOnlyList<SemanticBasicBlock> blocks = graph.Blocks
            .OrderBy(block => block.Ordinal)
            .Select(block => ProjectBlock(block, unit, typeRegistry, captureRegistry))
            .ToArray();
        return new SemanticControlFlowGraph(
            SemanticSymbolProjector.GetSymbolId(method),
            blocks.Single(block => block.Kind == "entry").Ordinal,
            blocks.Single(block => block.Kind == "exit").Ordinal,
            blocks);
    }

    private static SemanticBasicBlock ProjectBlock(
        BasicBlock block,
        SemanticCompilationUnit unit,
        SemanticTypeRegistry typeRegistry,
        SemanticCaptureRegistry captureRegistry)
    {
        return new SemanticBasicBlock(
            block.Ordinal,
            MapBlockKind(block.Kind),
            block.IsReachable,
            MapConditionKind(block.ConditionKind),
            block.Operations
                .Select(operation => SemanticOperationProjector.ProjectControlFlowOperation(
                    operation,
                    unit,
                    typeRegistry,
                    captureRegistry))
                .ToArray(),
            block.BranchValue is { } branchValue
                ? SemanticOperationProjector.ProjectControlFlowOperation(
                    branchValue,
                    unit,
                    typeRegistry,
                    captureRegistry)
                : null,
            block.Predecessors
                .Select(ProjectEdge)
                .OrderBy(edge => edge.SourceBlockOrdinal)
                .ThenBy(edge => edge.DestinationBlockOrdinal)
                .ThenBy(edge => edge.Kind, StringComparer.Ordinal)
                .ThenBy(edge => edge.Semantics, StringComparer.Ordinal)
                .ToArray(),
            GetBranches(block)
                .Select(ProjectEdge)
                .OrderBy(edge => edge.SourceBlockOrdinal)
                .ThenBy(edge => edge.DestinationBlockOrdinal)
                .ThenBy(edge => edge.Kind, StringComparer.Ordinal)
                .ThenBy(edge => edge.Semantics, StringComparer.Ordinal)
                .ToArray());
    }

    private static IEnumerable<ControlFlowBranch> GetAllBranches(BasicBlock block)
    {
        if (block.FallThroughSuccessor is { } fallThrough)
        {
            yield return fallThrough;
        }

        if (block.ConditionalSuccessor is { } conditional)
        {
            yield return conditional;
        }
    }

    private static IEnumerable<ControlFlowBranch> GetBranches(BasicBlock block)
    {
        foreach (ControlFlowBranch branch in GetAllBranches(block))
        {
            if (branch.Destination is not null)
            {
                yield return branch;
            }
        }
    }

    private static SemanticControlFlowEdge ProjectEdge(ControlFlowBranch branch)
    {
        return new SemanticControlFlowEdge(
            branch.Source.Ordinal,
            branch.Destination!.Ordinal,
            branch.IsConditionalSuccessor ? "conditional" : "fallthrough",
            MapBranchSemantics(branch.Semantics));
    }

    private static string MapBlockKind(BasicBlockKind kind)
    {
        return kind switch
        {
            BasicBlockKind.Entry => "entry",
            BasicBlockKind.Exit => "exit",
            BasicBlockKind.Block => "block",
            _ => "roslyn:" + kind.ToString().ToLowerInvariant(),
        };
    }

    private static string MapConditionKind(ControlFlowConditionKind kind)
    {
        return kind switch
        {
            ControlFlowConditionKind.None => "none",
            ControlFlowConditionKind.WhenFalse => "when_false",
            ControlFlowConditionKind.WhenTrue => "when_true",
            _ => "roslyn:" + kind.ToString().ToLowerInvariant(),
        };
    }

    private static string MapBranchSemantics(ControlFlowBranchSemantics semantics)
    {
        return semantics switch
        {
            ControlFlowBranchSemantics.None => "none",
            ControlFlowBranchSemantics.Regular => "regular",
            ControlFlowBranchSemantics.Return => "return",
            ControlFlowBranchSemantics.ProgramTermination => "program_termination",
            ControlFlowBranchSemantics.StructuredExceptionHandling => "structured_exception_handling",
            ControlFlowBranchSemantics.Throw => "throw",
            ControlFlowBranchSemantics.Rethrow => "rethrow",
            ControlFlowBranchSemantics.Error => "error",
            _ => "roslyn:" + semantics.ToString().ToLowerInvariant(),
        };
    }

    private static SemanticDiagnostic CreateInvalidGraphDiagnostic(SemanticSpan span)
    {
        return CreateDiagnostic(
            "ASCS3004",
            "Roslyn could not create a valid control-flow graph for this executable body.",
            span);
    }

    private static SemanticDiagnostic CreateDiagnostic(string code, string message, SemanticSpan span)
    {
        return new SemanticDiagnostic(code, "error", message, span);
    }
}
