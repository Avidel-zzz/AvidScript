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
    private const int MaximumAssetPathUtf8Bytes = 1024;

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
        Dictionary<string, int> localDeclarationSegments = new(StringComparer.Ordinal);
        Dictionary<string, int> resultUseSegments = new(StringComparer.Ordinal);
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
                if (awaitSite!.ResultSymbolId is not null)
                {
                    resultUseSegments.Add(awaitSite.ResultSymbolId, segment.Ordinal + 1);
                }
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
                out SemanticAsyncStatement? projectedStatement))
            {
                valid = false;
                continue;
            }

            segment.Statements.Add(projectedStatement!);
            segment.Include(statement.Span);
            if (projectedStatement!.TargetSymbolId is not null)
            {
                localDeclarationSegments.Add(projectedStatement.TargetSymbolId, segment.Ordinal);
            }
        }

        if (diagnostics.Count != diagnosticsBeforeStatements)
        {
            valid = false;
        }

        IReadOnlyList<SemanticAsyncSegment> projectedSegments = segments
            .Select(segment => segment.Build(context.PrimaryUnit, declaration.Body!.CloseBraceToken.SpanStart))
            .ToArray();
        if (!ValidateLocalLifetimes(
            projectedSegments,
            localDeclarationSegments,
            resultUseSegments,
            diagnostics))
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
        out SemanticAsyncStatement? projected)
    {
        projected = null;
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
            diagnostics.Add(Error(
                "ASCS5402",
                "Controlled async segments permit only expression statements and initialized single-local declarations.",
                SemanticSpanFactory.Create(context.PrimaryUnit.SourceText, statement.Span)));
            return false;
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

        string? producerKind = GetProducerKind(context, invocation.TargetMethod);
        string? payloadKind = producerKind == "object_load"
            ? SemanticContinuationCallback.ObjectPayloadKind
            : producerKind is null
                ? null
                : SemanticContinuationCallback.NonePayloadKind;
        if (producerKind is null
            || invocation.TargetMethod.IsAsync
            || IsTaskLike(invocation.TargetMethod.ReturnType)
            || !HasExpectedAwaitResult(producerKind, awaitOperation.Type))
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
        int expectedArgumentCount = producerKind == "next_tick"
            ? 0
            : producerKind.StartsWith(BindingLatentProducerPrefix, StringComparison.Ordinal)
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

        if (producerKind == "object_load" && !ValidateAssetPath(arguments[0], out string? pathError))
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
            if (producerKind != "object_load"
                || semanticModel.GetDeclaredSymbol(result) is not ILocalSymbol local
                || !IsLoadedObject(local.Type))
            {
                diagnostics.Add(Error(
                    "ASCS5404",
                    "Only LoadObjectAsync may initialize a single AvidLoadedObject result local.",
                    SemanticSpanFactory.Create(context.PrimaryUnit.SourceText, result.Span)));
                return false;
            }

            resultSymbolId = SemanticSymbolProjector.GetSymbolId(local);
            resultTypeId = typeRegistry.Register(local.Type);
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
            producerKind,
            payloadKind!,
            projectedArguments,
            resultSymbolId,
            resultTypeId,
            SemanticSpanFactory.Create(context.PrimaryUnit.SourceText, awaitExpression.Span),
            projectedCancellationToken);
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
        bool validContract = generatedReferenceMethod
            && !method.IsStatic
            && method.Name == "WithCancellation"
            && method.Arity == 0
            && !method.ContainingType.IsGenericType
            && owner is DelayAwaitableTypeName or ObjectAwaitableTypeName
            && method.ReturnType.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat) == owner
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

    private static string? GetProducerKind(
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
            string? module = latentAttribute.ConstructorArguments.Length == 2
                ? latentAttribute.ConstructorArguments[0].Value as string
                : null;
            string? importName = latentAttribute.ConstructorArguments.Length == 2
                ? latentAttribute.ConstructorArguments[1].Value as string
                : null;
            bool valid = generatedReferenceMethod
                && method.Arity == 0
                && !method.ContainingType.IsGenericType
                && method.Parameters.All(parameter => parameter.RefKind == RefKind.None)
                && !string.IsNullOrWhiteSpace(module)
                && !string.IsNullOrWhiteSpace(importName)
                && !module.Contains("|", StringComparison.Ordinal)
                && !importName.Contains("|", StringComparison.Ordinal);
            return valid
                ? $"{BindingLatentProducerPrefix}{module}|{importName}"
                : null;
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
            return "delay";
        }

        if (owner == ContinuationsTypeName
            && method.Name == "NextTickAsync"
            && method.Arity == 0
            && !method.ContainingType.IsGenericType
            && method.Parameters.Length == 0)
        {
            return "next_tick";
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
                ? "object_load"
                : null;
    }

    private static bool HasExpectedAwaitResult(string producerKind, ITypeSymbol? resultType)
    {
        return producerKind == "object_load"
            ? resultType is not null && IsLoadedObject(resultType)
            : resultType?.SpecialType == SpecialType.System_Void;
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

    private static bool ContainsAsyncHelperOrTask(IOperation operation)
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

    private static bool ValidateLocalLifetimes(
        IReadOnlyList<SemanticAsyncSegment> segments,
        IReadOnlyDictionary<string, int> localDeclarationSegments,
        IReadOnlyDictionary<string, int> resultUseSegments,
        ICollection<SemanticDiagnostic> diagnostics)
    {
        bool valid = true;
        foreach (SemanticAsyncSegment segment in segments)
        {
            IEnumerable<SemanticOperation> operations = segment.Statements
                .Select(statement => statement.Operation)
                .Concat(segment.AwaitSite?.Arguments ?? Array.Empty<SemanticOperation>());
            if (segment.AwaitSite?.CancellationToken is { } cancellationToken)
            {
                operations = operations.Append(cancellationToken);
            }
            foreach (SemanticOperation operation in operations.SelectMany(EnumerateOperations))
            {
                if (operation.SymbolId is not { } symbolId)
                {
                    continue;
                }

                if (localDeclarationSegments.TryGetValue(symbolId, out int declarationSegment)
                    && declarationSegment != segment.Ordinal)
                {
                    diagnostics.Add(Error(
                        "ASCS5405",
                        $"Local '{symbolId}' cannot cross a controlled await boundary.",
                        operation.Span));
                    valid = false;
                }
                else if (resultUseSegments.TryGetValue(symbolId, out int resultSegment)
                    && resultSegment != segment.Ordinal)
                {
                    diagnostics.Add(Error(
                        "ASCS5406",
                        $"Async object result '{symbolId}' is available only in the immediately following segment.",
                        operation.Span));
                    valid = false;
                }
            }
        }

        return valid;
    }

    private static bool IsLoadedObject(ITypeSymbol type)
    {
        return string.Equals(
            type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat),
            LoadedObjectTypeName,
            StringComparison.Ordinal);
    }

    private static bool AllOperationsSupported(SemanticOperation operation)
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
