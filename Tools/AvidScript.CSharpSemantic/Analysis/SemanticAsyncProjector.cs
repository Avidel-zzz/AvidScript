using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Operations;
using Microsoft.CodeAnalysis.Text;

namespace AvidScript.CSharpSemantic;

internal sealed record SemanticAsyncProjection(
    IReadOnlyList<SemanticAsyncMethod> Methods,
    IReadOnlySet<string> ControlledMethodSymbolIds,
    IReadOnlyList<SemanticDiagnostic> Diagnostics);

internal static class SemanticAsyncProjector
{
    private const string ContinuationsTypeName =
        "global::AvidScript.AvidContinuations";
    private const string AssetsTypeName =
        "global::AvidScript.AvidAssets";
    private const string LoadedObjectTypeName =
        "global::AvidScript.AvidLoadedObject";
    private const string CancellationTokenTypeName =
        "global::AvidScript.AvidCancellationToken";
    private const string DelayAwaitableTypeName =
        "global::AvidScript.AvidDelayAwaitable";
    private const string ObjectAwaitableTypeName =
        "global::AvidScript.AvidObjectAwaitable";
    private const string LatentAttributeName =
        "global::AvidScript.AvidLatentAttribute";
    private const string BindingLatentProducerPrefix = "binding_latent|";
    private const int MaximumAwaitsPerMethod = 16;
    private const int MaximumAwaitsPerModule = 64;
    private const int MaximumStateSlotsPerAwait = 64;
    private const int MaximumAssetPathUtf8Bytes = 1024;

    private sealed record ProducerContract(
        string Kind,
        string PayloadKind,
        int BindingOrdinal = -1,
        string? PayloadDescriptorTypeId = null,
        ITypeSymbol? PayloadValueType = null);

    public static SemanticAsyncProjection Project(
        SemanticCompilationContext context,
        SemanticTypeRegistry typeRegistry,
        IReadOnlyList<SemanticCallable> callables)
    {
        SemanticModel semanticModel = context.Compilation.GetSemanticModel(
            context.PrimaryUnit.SyntaxTree,
            ignoreAccessibility: false);
        IReadOnlyDictionary<string, SemanticCallable> callablesById = callables
            .ToDictionary(callable => callable.MethodSymbolId, StringComparer.Ordinal);
        List<SemanticAsyncMethod> methods = new();
        HashSet<string> controlledMethodIds = new(StringComparer.Ordinal);
        List<SemanticDiagnostic> diagnostics = new();
        int nextCallbackId = SemanticContinuationProjector.CompilerCallbackIdStart;

        MethodDeclarationSyntax[] declarations = context.PrimaryUnit.SyntaxTree
            .GetRoot()
            .DescendantNodes()
            .OfType<MethodDeclarationSyntax>()
            .Where(declaration => semanticModel.GetDeclaredSymbol(declaration) is IMethodSymbol { IsAsync: true })
            .OrderBy(declaration => declaration.SpanStart)
            .ToArray();

        foreach (MethodDeclarationSyntax declaration in declarations)
        {
            if (semanticModel.GetDeclaredSymbol(declaration) is not IMethodSymbol method)
            {
                continue;
            }

            int callbackStart = nextCallbackId;
            if (TryProjectMethod(
                context,
                semanticModel,
                declaration,
                method,
                callablesById,
                typeRegistry,
                diagnostics,
                ref nextCallbackId,
                out SemanticAsyncMethod? projected))
            {
                methods.Add(projected!);
                controlledMethodIds.Add(projected!.MethodSymbolId);
            }
            else
            {
                nextCallbackId = callbackStart;
            }
        }

        int awaitCount = methods.Sum(method => method.Segments.Count(segment => segment.AwaitSite is not null));
        if (awaitCount > MaximumAwaitsPerModule)
        {
            SemanticSpan span = methods
                .SelectMany(method => method.Segments)
                .Select(segment => segment.AwaitSite)
                .Where(site => site is not null)
                .Skip(MaximumAwaitsPerModule)
                .First()!
                .Span;
            diagnostics.Add(Error(
                "ASCS5408",
                $"The controlled async profile permits at most {MaximumAwaitsPerModule} await sites per module.",
                span));
            controlledMethodIds.Clear();
        }

        IReadOnlyList<SemanticDiagnostic> orderedDiagnostics = diagnostics
            .GroupBy(diagnostic =>
                (diagnostic.Code, diagnostic.Span.Start, diagnostic.Span.Length))
            .Select(group => group.First())
            .OrderBy(diagnostic => diagnostic.Span.Start)
            .ThenBy(diagnostic => diagnostic.Span.Length)
            .ThenBy(diagnostic => diagnostic.Code, StringComparer.Ordinal)
            .ToArray();
        return new SemanticAsyncProjection(
            orderedDiagnostics.Count == 0 ? methods : Array.Empty<SemanticAsyncMethod>(),
            controlledMethodIds,
            orderedDiagnostics);
    }

    private static bool TryProjectMethod(
        SemanticCompilationContext context,
        SemanticModel semanticModel,
        MethodDeclarationSyntax declaration,
        IMethodSymbol method,
        IReadOnlyDictionary<string, SemanticCallable> callablesById,
        SemanticTypeRegistry typeRegistry,
        ICollection<SemanticDiagnostic> diagnostics,
        ref int nextCallbackId,
        out SemanticAsyncMethod? projected)
    {
        projected = null;
        string methodSymbolId = SemanticSymbolProjector.GetSymbolId(method);
        SemanticSpan declarationSpan = SemanticSpanFactory.Create(
            context.PrimaryUnit.SourceText,
            declaration.Span);
        SemanticSpan identifierSpan = SemanticSpanFactory.Create(
            context.PrimaryUnit.SourceText,
            declaration.Identifier.Span);
        bool valid = callablesById.TryGetValue(methodSymbolId, out SemanticCallable? callable)
            && callable.Export is not null
            && method.DeclaredAccessibility == Accessibility.Public
            && method.IsStatic
            && method.ReturnsVoid
            && method.Parameters.Length == 0
            && !method.IsGenericMethod
            && !method.ContainingType.IsGenericType
            && !method.IsAbstract
            && !method.IsExtern
            && declaration.Body is not null
            && declaration.ExpressionBody is null;
        if (!valid)
        {
            diagnostics.Add(Error(
                "ASCS5401",
                $"Async method '{method.Name}' must be a zero-parameter public static non-generic block-bodied async void export.",
                identifierSpan));
            return false;
        }

        AwaitExpressionSyntax[] awaits = declaration.Body!.DescendantNodes()
            .OfType<AwaitExpressionSyntax>()
            .OrderBy(node => node.SpanStart)
            .ToArray();
        if (awaits.Length > MaximumAwaitsPerMethod)
        {
            diagnostics.Add(Error(
                "ASCS5407",
                $"Async method '{method.Name}' exceeds the {MaximumAwaitsPerMethod}-await method limit.",
                SemanticSpanFactory.Create(
                    context.PrimaryUnit.SourceText,
                    awaits[MaximumAwaitsPerMethod].Span)));
            return false;
        }

        List<AsyncSegmentBuilder> segments = new() { new AsyncSegmentBuilder(0) };
        int structuredNodeCount = 0;
        int diagnosticsBeforeStatements = diagnostics.Count;
        foreach (StatementSyntax statement in declaration.Body.Statements)
        {
            AsyncSegmentBuilder segment = segments[^1];
            if (TryGetDirectAwait(statement, out AwaitExpressionSyntax? awaitExpression, out VariableDeclaratorSyntax? result))
            {
                if (!TryProjectAwaitSite(
                    context,
                    semanticModel,
                    awaitExpression!,
                    result,
                    typeRegistry,
                    diagnostics,
                    ref nextCallbackId,
                    out SemanticAsyncAwaitSite? awaitSite))
                {
                    valid = false;
                    continue;
                }

                segment.AwaitSite = awaitSite;
                segment.Include(statement.Span);
                segments.Add(new AsyncSegmentBuilder(segment.Ordinal + 1));
                continue;
            }

            if (statement.DescendantNodesAndSelf().OfType<AwaitExpressionSyntax>().Any())
            {
                diagnostics.Add(Error(
                    "ASCS5402",
                    "Await expressions must be direct top-level statements or single-local initializers.",
                    SemanticSpanFactory.Create(context.PrimaryUnit.SourceText, statement.Span)));
                valid = false;
                continue;
            }

            if (!TryProjectStatement(
                context,
                semanticModel,
                statement,
                typeRegistry,
                diagnostics,
                ref structuredNodeCount,
                out SemanticAsyncStatement? projectedStatement))
            {
                valid = false;
                continue;
            }

            segment.Statements.Add(projectedStatement!);
            segment.Include(statement.Span);
        }

        if (diagnostics.Count != diagnosticsBeforeStatements)
        {
            valid = false;
        }

        IReadOnlyList<SemanticAsyncSegment> projectedSegments = segments
            .Select(segment => segment.Build(context.PrimaryUnit, declaration.Body!.CloseBraceToken.SpanStart))
            .ToArray();
        if (!TryAttachStateFrames(
            projectedSegments,
            diagnostics,
            out projectedSegments))
        {
            valid = false;
        }

        if (!valid)
        {
            return false;
        }

        projected = new SemanticAsyncMethod(
            methodSymbolId,
            callable!.Export!.Name,
            SemanticAsyncMethod.ReentrantZeroHeapCpsLowering,
            projectedSegments,
            declarationSpan);
        return true;
    }

    private static bool TryGetDirectAwait(
        StatementSyntax statement,
        out AwaitExpressionSyntax? awaitExpression,
        out VariableDeclaratorSyntax? result)
    {
        awaitExpression = null;
        result = null;
        if (statement is ExpressionStatementSyntax { Expression: AwaitExpressionSyntax expressionAwait })
        {
            awaitExpression = expressionAwait;
            return true;
        }

        if (statement is LocalDeclarationStatementSyntax local
            && local.Declaration.Variables.Count == 1
            && local.Declaration.Variables[0] is { Initializer.Value: AwaitExpressionSyntax localAwait } variable)
        {
            awaitExpression = localAwait;
            result = variable;
            return true;
        }

        return false;
    }

    private static bool TryProjectStatement(
        SemanticCompilationContext context,
        SemanticModel semanticModel,
        StatementSyntax statement,
        SemanticTypeRegistry typeRegistry,
        ICollection<SemanticDiagnostic> diagnostics,
        ref int structuredNodeCount,
        out SemanticAsyncStatement? projected)
    {
        projected = null;
        if (statement is IfStatementSyntax guard && IsEarlyReturnGuard(guard))
        {
            return TryProjectEarlyReturnGuard(
                context,
                semanticModel,
                guard,
                typeRegistry,
                diagnostics,
                out projected);
        }

        IOperation? operation;
        string? targetSymbolId = null;
        if (statement is ExpressionStatementSyntax expressionStatement)
        {
            operation = semanticModel.GetOperation(expressionStatement);
        }
        else if (statement is LocalDeclarationStatementSyntax local
            && local.Declaration.Variables.Count == 1
            && local.Declaration.Variables[0] is { Initializer: not null } variable
            && semanticModel.GetDeclaredSymbol(variable) is ILocalSymbol localSymbol)
        {
            operation = semanticModel.GetOperation(variable.Initializer!.Value);
            targetSymbolId = SemanticSymbolProjector.GetSymbolId(localSymbol);
        }
        else
        {
            return SemanticAsyncStructuredStatementProjector.TryProject(
                context,
                semanticModel,
                statement,
                typeRegistry,
                diagnostics,
                ref structuredNodeCount,
                out projected);
        }

        if (operation is null)
        {
            diagnostics.Add(Error(
                "ASCS5402",
                "Roslyn did not expose a stable operation for the async segment statement.",
                SemanticSpanFactory.Create(context.PrimaryUnit.SourceText, statement.Span)));
            return false;
        }

        if (ContainsAsyncHelperOrTask(operation))
        {
            diagnostics.Add(Error(
                "ASCS5403",
                "Task, ValueTask, arbitrary awaiters, and async helper call chains are outside the controlled async profile.",
                SemanticSpanFactory.Create(context.PrimaryUnit.SourceText, statement.Span)));
            return false;
        }

        int diagnosticsBeforeProjection = diagnostics.Count;
        SemanticOperation semanticOperation = SemanticOperationProjector.ProjectAsyncStatementOperation(
            operation,
            context.PrimaryUnit,
            typeRegistry,
            diagnostics);
        if (diagnostics.Count != diagnosticsBeforeProjection || !AllOperationsSupported(semanticOperation))
        {
            return false;
        }

        projected = new SemanticAsyncStatement(semanticOperation, targetSymbolId);
        return true;
    }

    private static bool IsEarlyReturnGuard(IfStatementSyntax guard)
    {
        ReturnStatementSyntax? returnStatement = guard.Statement switch
        {
            ReturnStatementSyntax directReturn => directReturn,
            BlockSyntax { Statements.Count: 1 } block =>
                block.Statements[0] as ReturnStatementSyntax,
            _ => null,
        };
        return guard.Else is null
            && returnStatement is { Expression: null };
    }

    private static bool TryProjectEarlyReturnGuard(
        SemanticCompilationContext context,
        SemanticModel semanticModel,
        IfStatementSyntax guard,
        SemanticTypeRegistry typeRegistry,
        ICollection<SemanticDiagnostic> diagnostics,
        out SemanticAsyncStatement? projected)
    {
        projected = null;
        ReturnStatementSyntax? returnStatement = guard.Statement switch
        {
            ReturnStatementSyntax directReturn => directReturn,
            BlockSyntax { Statements.Count: 1 } block =>
                block.Statements[0] as ReturnStatementSyntax,
            _ => null,
        };
        if (guard.Else is not null
            || returnStatement is null
            || returnStatement.Expression is not null)
        {
            diagnostics.Add(Error(
                "ASCS5410",
                "Controlled async guards must use a top-level 'if (condition) return;' without else or branch side effects.",
                SemanticSpanFactory.Create(context.PrimaryUnit.SourceText, guard.Span)));
            return false;
        }

        IOperation? conditionOperation = semanticModel.GetOperation(guard.Condition);
        if (conditionOperation?.Type?.SpecialType != SpecialType.System_Boolean
            || ContainsAsyncHelperOrTask(conditionOperation))
        {
            diagnostics.Add(Error(
                "ASCS5410",
                "Controlled async guard conditions must be supported synchronous bool expressions.",
                SemanticSpanFactory.Create(context.PrimaryUnit.SourceText, guard.Condition.Span)));
            return false;
        }

        int diagnosticsBeforeProjection = diagnostics.Count;
        SemanticOperation condition = SemanticOperationProjector.ProjectAsyncStatementOperation(
            conditionOperation,
            context.PrimaryUnit,
            typeRegistry,
            diagnostics);
        if (diagnostics.Count != diagnosticsBeforeProjection || !AllOperationsSupported(condition))
        {
            return false;
        }

        SemanticSpan span = SemanticSpanFactory.Create(
            context.PrimaryUnit.SourceText,
            guard.Span);
        SemanticOperation operation = new(
            Kind: SemanticAsyncMethod.EarlyReturnGuardOperationKind,
            IsSupported: true,
            OperatorKind: null,
            IsChecked: false,
            IsLifted: false,
            IsPostfix: false,
            IsTryCast: false,
            TypeId: "type:void",
            SymbolId: null,
            TypeArgumentIds: Array.Empty<string>(),
            Constant: null,
            Conversion: null,
            InputConversion: null,
            OutputConversion: null,
            CaptureId: null,
            Span: span,
            Children: new[] { condition });
        projected = new SemanticAsyncStatement(operation, null);
        return true;
    }

    private static bool TryProjectAwaitSite(
        SemanticCompilationContext context,
        SemanticModel semanticModel,
        AwaitExpressionSyntax awaitExpression,
        VariableDeclaratorSyntax? result,
        SemanticTypeRegistry typeRegistry,
        ICollection<SemanticDiagnostic> diagnostics,
        ref int nextCallbackId,
        out SemanticAsyncAwaitSite? projected)
    {
        projected = null;
        if (semanticModel.GetOperation(awaitExpression) is not IAwaitOperation awaitOperation
            || awaitOperation.Operation is not IInvocationOperation candidateInvocation)
        {
            diagnostics.Add(Error(
                "ASCS5403",
                "Controlled await requires a direct built-in or generated latent producer invocation.",
                SemanticSpanFactory.Create(context.PrimaryUnit.SourceText, awaitExpression.Span)));
            return false;
        }

        IInvocationOperation invocation = candidateInvocation;
        IOperation? cancellationTokenOperation = null;
        if (TryUnwrapCancellationMarker(
                context,
                candidateInvocation,
                out IInvocationOperation? wrappedProducer,
                out IOperation? tokenOperation))
        {
            invocation = wrappedProducer!;
            cancellationTokenOperation = tokenOperation;
        }

        ProducerContract? producer = GetProducerContract(context, invocation.TargetMethod);
        if (producer is null
            || invocation.TargetMethod.IsAsync
            || IsTaskLike(invocation.TargetMethod.ReturnType)
            || !HasExpectedAwaitResult(producer, awaitOperation.Type))
        {
            diagnostics.Add(Error(
                "ASCS5403",
                "Controlled await requires an exact built-in or generated latent producer contract.",
                SemanticSpanFactory.Create(context.PrimaryUnit.SourceText, awaitExpression.Span)));
            return false;
        }

        IArgumentOperation[] arguments = invocation.Arguments
            .OrderBy(argument => argument.Parameter?.Ordinal ?? int.MaxValue)
            .ToArray();
        int expectedArgumentCount = producer.Kind == "next_tick"
            ? 0
            : producer.Kind.StartsWith(BindingLatentProducerPrefix, StringComparison.Ordinal)
                ? invocation.TargetMethod.Parameters.Length
                : 1;
        if (arguments.Length != expectedArgumentCount)
        {
            diagnostics.Add(Error(
                "ASCS5404",
                $"Controlled async producer '{invocation.TargetMethod.Name}' has an invalid argument shape.",
                SemanticSpanFactory.Create(context.PrimaryUnit.SourceText, awaitExpression.Span)));
            return false;
        }

        if (producer.Kind == "object_load" && !ValidateAssetPath(arguments[0], out string? pathError))
        {
            diagnostics.Add(Error(
                "ASCS5404",
                pathError!,
                SemanticSpanFactory.Create(context.PrimaryUnit.SourceText, arguments[0].Syntax.Span)));
            return false;
        }

        string? resultSymbolId = null;
        string? resultTypeId = null;
        if (result is not null)
        {
            if (semanticModel.GetDeclaredSymbol(result) is not ILocalSymbol local
                || producer.PayloadKind == SemanticContinuationCallback.ObjectPayloadKind
                    && !IsLoadedObject(local.Type)
                || producer.PayloadKind == SemanticContinuationCallback.ResultSlotPayloadKind
                    && !IsOutcomeOf(local.Type, producer.PayloadValueType)
                || producer.PayloadKind == SemanticContinuationCallback.NonePayloadKind)
            {
                diagnostics.Add(Error(
                    "ASCS5404",
                    "The controlled await result local must exactly match its producer payload contract.",
                    SemanticSpanFactory.Create(context.PrimaryUnit.SourceText, result.Span)));
                return false;
            }

            resultSymbolId = SemanticSymbolProjector.GetSymbolId(local);
            resultTypeId = typeRegistry.Register(local.Type);
        }
        else if (producer.PayloadKind == SemanticContinuationCallback.ResultSlotPayloadKind)
        {
            resultTypeId = typeRegistry.Register(awaitOperation.Type!);
        }

        List<SemanticOperation> projectedArguments = new(arguments.Length);
        SemanticOperation? projectedCancellationToken = null;
        int diagnosticsBeforeProjection = diagnostics.Count;
        foreach (IArgumentOperation argument in arguments)
        {
            projectedArguments.Add(SemanticOperationProjector.ProjectAsyncStatementOperation(
                argument.Value,
                context.PrimaryUnit,
                typeRegistry,
                diagnostics));
        }
        if (cancellationTokenOperation is not null)
        {
            projectedCancellationToken = SemanticOperationProjector.ProjectAsyncStatementOperation(
                cancellationTokenOperation,
                context.PrimaryUnit,
                typeRegistry,
                diagnostics);
        }
        if (diagnostics.Count != diagnosticsBeforeProjection
            || projectedArguments.Any(argument => !AllOperationsSupported(argument))
            || projectedCancellationToken is not null
                && !AllOperationsSupported(projectedCancellationToken))
        {
            return false;
        }

        projected = new SemanticAsyncAwaitSite(
            nextCallbackId++,
            producer.Kind,
            producer.PayloadKind,
            projectedArguments,
            resultSymbolId,
            resultTypeId,
            SemanticSpanFactory.Create(context.PrimaryUnit.SourceText, awaitExpression.Span),
            projectedCancellationToken,
            producer.BindingOrdinal,
            producer.PayloadDescriptorTypeId,
            producer.PayloadValueType is null
                ? null
                : typeRegistry.Register(producer.PayloadValueType));
        return true;
    }

    private static bool TryUnwrapCancellationMarker(
        SemanticCompilationContext context,
        IInvocationOperation marker,
        out IInvocationOperation? producer,
        out IOperation? cancellationToken)
    {
        producer = null;
        cancellationToken = null;
        IMethodSymbol method = marker.TargetMethod;
        string owner = method.ContainingType.ToDisplayString(
            SymbolDisplayFormat.FullyQualifiedFormat);
        bool generatedReferenceMethod = method.DeclaringSyntaxReferences.Length == 1
            && method.DeclaringSyntaxReferences[0].SyntaxTree != context.PrimaryUnit.SyntaxTree;
        bool supportedOwner = owner is DelayAwaitableTypeName or ObjectAwaitableTypeName
            || IsAvidGenericType(method.ContainingType, "AvidOutcomeAwaitable", out _);
        bool validContract = generatedReferenceMethod
            && !method.IsStatic
            && method.Name == "WithCancellation"
            && method.Arity == 0
            && supportedOwner
            && SymbolEqualityComparer.Default.Equals(method.ReturnType, method.ContainingType)
            && method.Parameters.Length == 1
            && method.Parameters[0].RefKind == RefKind.None
            && method.Parameters[0].Type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat)
                == CancellationTokenTypeName
            && marker.Arguments.Length == 1;
        if (!validContract
            || UnwrapTransparentOperation(marker.Instance) is not IInvocationOperation wrappedProducer)
        {
            return false;
        }

        producer = wrappedProducer;
        cancellationToken = marker.Arguments[0].Value;
        return true;
    }

    private static IOperation? UnwrapTransparentOperation(IOperation? operation)
    {
        while (operation is IConversionOperation { IsImplicit: true } conversion)
        {
            operation = conversion.Operand;
        }
        while (operation is IParenthesizedOperation parenthesized)
        {
            operation = parenthesized.Operand;
        }
        return operation;
    }

    private static ProducerContract? GetProducerContract(
        SemanticCompilationContext context,
        IMethodSymbol method)
    {
        string owner = method.ContainingType.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat);
        if (!method.IsStatic)
        {
            return null;
        }

        AttributeData? latentAttribute = method.GetAttributes().SingleOrDefault(attribute =>
            attribute.AttributeClass?.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat)
                == LatentAttributeName);
        if (latentAttribute is not null)
        {
            bool generatedReferenceMethod = method.DeclaringSyntaxReferences.Length == 1
                && method.DeclaringSyntaxReferences[0].SyntaxTree != context.PrimaryUnit.SyntaxTree;
            int argumentCount = latentAttribute.ConstructorArguments.Length;
            string? module = argumentCount is 2 or 4
                ? latentAttribute.ConstructorArguments[0].Value as string
                : null;
            string? importName = argumentCount is 2 or 4
                ? latentAttribute.ConstructorArguments[1].Value as string
                : null;
            int bindingOrdinal = argumentCount == 4
                && latentAttribute.ConstructorArguments[2].Value is int ordinal
                    ? ordinal
                    : -1;
            string? payloadDescriptorTypeId = argumentCount == 4
                ? latentAttribute.ConstructorArguments[3].Value as string
                : null;
            ITypeSymbol? payloadValueType = argumentCount == 4
                && IsAvidGenericType(method.ReturnType, "AvidOutcomeAwaitable", out ITypeSymbol? valueType)
                    ? valueType
                    : null;
            bool valid = generatedReferenceMethod
                && method.Arity == 0
                && !method.ContainingType.IsGenericType
                && method.Parameters.All(parameter => parameter.RefKind == RefKind.None)
                && !string.IsNullOrWhiteSpace(module)
                && !string.IsNullOrWhiteSpace(importName)
                && !module.Contains("|", StringComparison.Ordinal)
                && !importName.Contains("|", StringComparison.Ordinal)
                && (argumentCount == 2
                    && method.ReturnType.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat)
                        == DelayAwaitableTypeName
                    || argumentCount == 4
                        && bindingOrdinal >= 0
                        && IsLowerSha256(payloadDescriptorTypeId)
                        && payloadValueType is not null);
            if (!valid)
            {
                return null;
            }
            string kind = $"{BindingLatentProducerPrefix}{module}|{importName}";
            return argumentCount == 4
                ? new ProducerContract(
                    kind,
                    SemanticContinuationCallback.ResultSlotPayloadKind,
                    bindingOrdinal,
                    payloadDescriptorTypeId,
                    payloadValueType)
                : new ProducerContract(kind, SemanticContinuationCallback.NonePayloadKind);
        }

        if (owner == ContinuationsTypeName
            && method.Name == "DelayAsync"
            && method.Arity == 0
            && !method.ContainingType.IsGenericType
            && method.Parameters.Length == 1
            && method.Parameters[0].RefKind == RefKind.None
            && !method.Parameters[0].IsOptional
            && !method.Parameters[0].IsParams
            && method.Parameters[0].Type.SpecialType == SpecialType.System_Single)
        {
            return new ProducerContract("delay", SemanticContinuationCallback.NonePayloadKind);
        }

        if (owner == ContinuationsTypeName
            && method.Name == "NextTickAsync"
            && method.Arity == 0
            && !method.ContainingType.IsGenericType
            && method.Parameters.Length == 0)
        {
            return new ProducerContract("next_tick", SemanticContinuationCallback.NonePayloadKind);
        }

        return owner == AssetsTypeName
            && method.Name == "LoadObjectAsync"
            && method.Arity == 0
            && !method.ContainingType.IsGenericType
            && method.Parameters.Length == 1
            && method.Parameters[0].RefKind == RefKind.None
            && !method.Parameters[0].IsOptional
            && !method.Parameters[0].IsParams
            && method.Parameters[0].Type.SpecialType == SpecialType.System_String
                ? new ProducerContract(
                    "object_load",
                    SemanticContinuationCallback.ObjectPayloadKind)
                : null;
    }

    private static bool HasExpectedAwaitResult(
        ProducerContract producer,
        ITypeSymbol? resultType)
    {
        return producer.PayloadKind switch
        {
            SemanticContinuationCallback.ObjectPayloadKind =>
                resultType is not null && IsLoadedObject(resultType),
            SemanticContinuationCallback.ResultSlotPayloadKind =>
                IsOutcomeOf(resultType, producer.PayloadValueType),
            _ => resultType?.SpecialType == SpecialType.System_Void,
        };
    }

    private static bool IsOutcomeOf(ITypeSymbol? type, ITypeSymbol? valueType)
    {
        return IsAvidGenericType(type, "AvidOutcome", out ITypeSymbol? argument)
            && valueType is not null
            && SymbolEqualityComparer.Default.Equals(argument, valueType);
    }

    private static bool IsAvidGenericType(
        ITypeSymbol? type,
        string name,
        out ITypeSymbol? argument)
    {
        argument = null;
        if (type is not INamedTypeSymbol named
            || !named.IsGenericType
            || named.Arity != 1
            || named.Name != name
            || named.ContainingNamespace.ToDisplayString() != "AvidScript")
        {
            return false;
        }
        argument = named.TypeArguments[0];
        return true;
    }

    private static bool IsLowerSha256(string? value)
    {
        return value is { Length: 64 }
            && value.All(character => character is >= '0' and <= '9'
                or >= 'a' and <= 'f');
    }

    private static bool ValidateAssetPath(IArgumentOperation argument, out string? error)
    {
        if (argument.Value.ConstantValue is not { HasValue: true, Value: string assetPath }
            || assetPath.Length == 0)
        {
            error = "LoadObjectAsync requires a compile-time nonempty asset path.";
            return false;
        }

        if (assetPath.IndexOf('\0') >= 0 || !IsCanonicalLookingAssetPath(assetPath))
        {
            error = "LoadObjectAsync asset path must be canonical-looking and contain no NUL character.";
            return false;
        }

        if (Encoding.UTF8.GetByteCount(assetPath) > MaximumAssetPathUtf8Bytes)
        {
            error = $"LoadObjectAsync asset path must not exceed {MaximumAssetPathUtf8Bytes} UTF-8 bytes.";
            return false;
        }

        error = null;
        return true;
    }

    private static bool IsCanonicalLookingAssetPath(string assetPath)
    {
        if (assetPath.Length < 4
            || assetPath[0] != '/'
            || assetPath[1] == '/'
            || assetPath.IndexOf('\\') >= 0
            || assetPath.Any(character => char.IsControl(character) || char.IsWhiteSpace(character)))
        {
            return false;
        }

        int lastSlash = assetPath.LastIndexOf('/');
        int objectSeparator = assetPath.IndexOf('.', lastSlash + 1);
        return lastSlash > 0
            && objectSeparator > lastSlash + 1
            && objectSeparator < assetPath.Length - 1
            && assetPath.IndexOf('.', objectSeparator + 1) < 0
            && !assetPath.Contains("//", StringComparison.Ordinal);
    }

    internal static bool ContainsAsyncHelperOrTask(IOperation operation)
    {
        return EnumerateOperations(operation).Any(item =>
            IsTaskLike(item.Type)
            || IsAwaitableType(item.Type)
            || item is IInvocationOperation invocation
                && (invocation.TargetMethod.IsAsync || IsTaskLike(invocation.TargetMethod.ReturnType)));
    }

    private static bool IsAwaitableType(ITypeSymbol? type)
    {
        return type is INamedTypeSymbol named
            && named.GetMembers("GetAwaiter")
                .OfType<IMethodSymbol>()
                .Any(method => !method.IsStatic && method.Parameters.Length == 0);
    }

    private static bool IsTaskLike(ITypeSymbol? type)
    {
        if (type is null)
        {
            return false;
        }

        return type.OriginalDefinition is INamedTypeSymbol named
            && (named.Name is "Task" or "ValueTask")
            && named.ContainingNamespace.ToDisplayString() == "System.Threading.Tasks";
    }

    private static bool TryAttachStateFrames(
        IReadOnlyList<SemanticAsyncSegment> segments,
        ICollection<SemanticDiagnostic> diagnostics,
        out IReadOnlyList<SemanticAsyncSegment> projectedSegments)
    {
        projectedSegments = segments;
        SemanticAsyncStateFlowAnalysis analysis = SemanticAsyncStateFlowAnalyzer.Analyze(segments);
        foreach (SemanticAsyncStateFlowIssue issue in analysis.Issues)
        {
            diagnostics.Add(Error("ASCS5413", issue.Message, issue.Span));
        }
        if (analysis.Issues.Count > 0)
        {
            return false;
        }

        List<SemanticAsyncSegment> framed = new(segments.Count);
        foreach (SemanticAsyncSegment segment in segments)
        {
            if (segment.AwaitSite is not { } awaitSite)
            {
                framed.Add(segment);
                continue;
            }

            IReadOnlyList<SemanticAsyncStateSlot> slots =
                analysis.SlotsByAwaitSegment.GetValueOrDefault(segment.Ordinal)
                ?? Array.Empty<SemanticAsyncStateSlot>();
            if (slots.Count > MaximumStateSlotsPerAwait)
            {
                diagnostics.Add(Error(
                    "ASCS5414",
                    $"Async await site exceeds the {MaximumStateSlotsPerAwait}-slot state-frame limit.",
                    awaitSite.Span));
                return false;
            }

            SemanticAsyncStateFrame? frame = slots.Count == 0
                ? null
                : new SemanticAsyncStateFrame(
                    $"type:synthetic:async_state:{awaitSite.CallbackId}",
                    slots);
            framed.Add(segment with
            {
                AwaitSite = awaitSite with { StateFrame = frame },
            });
        }
        projectedSegments = framed;
        return true;
    }

    private static bool IsLoadedObject(ITypeSymbol type)
    {
        return string.Equals(
            type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat),
            LoadedObjectTypeName,
            StringComparison.Ordinal);
    }

    internal static bool AllOperationsSupported(SemanticOperation operation)
    {
        return operation.IsSupported && operation.Children.All(AllOperationsSupported);
    }

    private static IEnumerable<IOperation> EnumerateOperations(IOperation operation)
    {
        yield return operation;
        foreach (IOperation child in operation.ChildOperations)
        {
            foreach (IOperation descendant in EnumerateOperations(child))
            {
                yield return descendant;
            }
        }
    }

    private static IEnumerable<SemanticOperation> EnumerateOperations(SemanticOperation operation)
    {
        yield return operation;
        foreach (SemanticOperation child in operation.Children)
        {
            foreach (SemanticOperation descendant in EnumerateOperations(child))
            {
                yield return descendant;
            }
        }
    }

    private static SemanticDiagnostic Error(string code, string message, SemanticSpan span)
    {
        return new SemanticDiagnostic(code, "error", message, span);
    }

    private sealed class AsyncSegmentBuilder
    {
        private int start = int.MaxValue;
        private int end;

        public AsyncSegmentBuilder(int ordinal)
        {
            Ordinal = ordinal;
        }

        public int Ordinal { get; }

        public List<SemanticAsyncStatement> Statements { get; } = new();

        public SemanticAsyncAwaitSite? AwaitSite { get; set; }

        public void Include(TextSpan span)
        {
            start = Math.Min(start, span.Start);
            end = Math.Max(end, span.End);
        }

        public SemanticAsyncSegment Build(SemanticCompilationUnit unit, int emptyPosition)
        {
            TextSpan span = start == int.MaxValue
                ? new TextSpan(emptyPosition, 0)
                : TextSpan.FromBounds(start, end);
            return new SemanticAsyncSegment(
                Ordinal,
                Statements.ToArray(),
                AwaitSite,
                SemanticSpanFactory.Create(unit.SourceText, span));
        }
    }
}
