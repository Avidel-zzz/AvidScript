using System;
using System.Collections.Generic;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Operations;

namespace AvidScript.CSharpSemantic;

// Establishes local/scope identity from Roslyn. A representative call supplies
// only the exact Task result type during await projection, never its identity.
internal static class SemanticAsyncTaskLocalProjector
{
    internal sealed record Plan(VariableDeclaratorSyntax[] Declarations,
        string[] LocalIds, IInvocationOperation Representative);

    public static bool RequiresLifetime(SemanticCompilationContext context,
        SemanticModel model, BlockSyntax body)
    {
        if (Declarations(context, model, body).Length == 0) return false;
        // Schema selection belongs to the module. Once one method needs the new
        // contract, every Task-owning method records its complete lifetime plan.
        return context.PrimaryUnit.SyntaxTree.GetRoot().DescendantNodes().OfType<MethodDeclarationSyntax>()
            .Any(method => method.Body is { } candidate && RequiresNewContract(context, model, candidate));
    }

    private static bool RequiresNewContract(SemanticCompilationContext context,
        SemanticModel model, BlockSyntax body)
    {
        var declarations = Declarations(context, model, body);
        return declarations.Any(variable => variable.Initializer is null
                || variable.Ancestors().TakeWhile(node => node != body)
                    .Any(node => node is ForStatementSyntax or ForEachStatementSyntax
                        or WhileStatementSyntax or DoStatementSyntax))
            || body.DescendantNodes().OfType<AssignmentExpressionSyntax>().Any(assignment =>
                model.GetOperation(assignment.Left) is ILocalReferenceOperation reference
                && SemanticAsyncProjector.TryGetSupportedTaskResult(context.Compilation, reference.Type!, out _));
    }

    public static bool TryAnalyze(SemanticCompilationContext context, SemanticModel model,
        BlockSyntax body, out Plan? plan)
    {
        plan = null;
        var declarations = Declarations(context, model, body);
        if (declarations.Length is < 1 or > SemanticAsyncInvocationValidator.MaximumTaskLocalsPerMethod)
            return false;
        var locals = declarations.Select(variable => (ILocalSymbol)model.GetDeclaredSymbol(variable)!).ToArray();
        var ids = locals.Select(SemanticSymbolProjector.GetSymbolId).ToArray();
        var owned = ids.ToHashSet(StringComparer.Ordinal);
        IInvocationOperation? representative = null;
        bool IsOwned(ILocalSymbol local) => owned.Contains(SemanticSymbolProjector.GetSymbolId(local));
        bool IsValue(IOperation? value)
        {
            if (value is ILocalReferenceOperation alias) return IsOwned(alias.Local);
            if (value is not IInvocationOperation call
                || !SemanticAsyncProjector.TryGetSupportedTaskResult(context.Compilation, call.Type!, out _)
                || !call.TargetMethod.IsAsync || !call.TargetMethod.IsStatic
                || call.TargetMethod.IsGenericMethod || call.TargetMethod.ContainingType.IsGenericType
                || call.TargetMethod.IsVirtual || call.TargetMethod.IsOverride || call.TargetMethod.IsAbstract
                || call.TargetMethod.DeclaringSyntaxReferences.Length != 1
                || call.TargetMethod.DeclaringSyntaxReferences[0].SyntaxTree != context.PrimaryUnit.SyntaxTree
                || call.Arguments.Length != call.TargetMethod.Parameters.Length
                || call.Arguments.Where((argument, index) => argument.ArgumentKind != ArgumentKind.Explicit
                    || argument.Parameter?.Ordinal != index
                    || call.TargetMethod.Parameters[index].RefKind != RefKind.None).Any()) return false;
            representative ??= call;
            return true;
        }
        foreach (var variable in declarations)
        {
            if (variable.Parent?.Parent is not LocalDeclarationStatementSyntax
                    { Declaration.Variables.Count: 1, Parent: BlockSyntax } statement
                || statement.UsingKeyword.RawKind != 0
                || variable.Ancestors().TakeWhile(node => node != body).Any(node =>
                    node is AnonymousFunctionExpressionSyntax or LocalFunctionStatementSyntax)
                || variable.Initializer is { } initializer
                    && !IsValue((model.GetOperation(initializer) as IVariableInitializerOperation)?.Value)) return false;
        }
        foreach (var assignment in body.DescendantNodes().OfType<AssignmentExpressionSyntax>())
        {
            if (model.GetOperation(assignment.Left) is not ILocalReferenceOperation target
                || !IsOwned(target.Local)) continue;
            if (assignment.Parent is not ExpressionStatementSyntax
                || model.GetOperation(assignment) is not ISimpleAssignmentOperation operation
                || !IsValue(operation.Value)) return false;
        }
        foreach (var identifier in body.DescendantNodes().OfType<IdentifierNameSyntax>())
        {
            if (model.GetOperation(identifier) is not ILocalReferenceOperation reference
                || !IsOwned(reference.Local)) continue;
            if (identifier.Ancestors().TakeWhile(node => node != body).Any(node =>
                    node is AnonymousFunctionExpressionSyntax or LocalFunctionStatementSyntax)) return false;
            if (identifier.Parent is AwaitExpressionSyntax awaitSyntax && awaitSyntax.Expression == identifier) continue;
            if (identifier.Parent is EqualsValueClauseSyntax { Parent: VariableDeclaratorSyntax alias }
                && declarations.Contains(alias)) continue;
            if (identifier.Parent is AssignmentExpressionSyntax assignment
                && assignment.Parent is ExpressionStatementSyntax
                && model.GetOperation(assignment) is ISimpleAssignmentOperation
                    { Target: ILocalReferenceOperation target } && IsOwned(target.Local)) continue;
            return false;
        }
        if (representative is null) return false;
        plan = new(declarations, ids, representative);
        return true;
    }

    public static IReadOnlyList<SemanticAsyncTaskLocalLifetime>? ProjectLifetimes(
        SemanticCompilationContext context, SemanticModel model, BlockSyntax body,
        IReadOnlyList<SemanticAsyncLexicalScope> scopes)
    {
        if (!RequiresLifetime(context, model, body)) return null;
        if (!TryAnalyze(context, model, body, out var plan)) return Array.Empty<SemanticAsyncTaskLocalLifetime>();
        List<SemanticAsyncTaskLocalLifetime> lifetimes = new();
        for (int index = 0; index < plan!.Declarations.Length; index++)
        {
            var block = (BlockSyntax)plan.Declarations[index].Parent!.Parent!.Parent!;
            var scope = block == body ? scopes.SingleOrDefault(candidate => candidate.Kind == "activation")
                : scopes.SingleOrDefault(candidate => candidate.Kind == "block_entry"
                    && candidate.Span.Start == block.Span.Start && candidate.Span.Length == block.Span.Length);
            if (scope is null) return Array.Empty<SemanticAsyncTaskLocalLifetime>();
            lifetimes.Add(new(plan.LocalIds[index], scope.Id));
        }
        return lifetimes;
    }

    private static VariableDeclaratorSyntax[] Declarations(SemanticCompilationContext context,
        SemanticModel model, BlockSyntax body) => body.DescendantNodes().OfType<VariableDeclaratorSyntax>()
            .Where(variable => model.GetDeclaredSymbol(variable) is ILocalSymbol local
                && SemanticAsyncProjector.TryGetSupportedTaskResult(context.Compilation, local.Type, out _))
            .OrderBy(variable => variable.SpanStart).ToArray();
}
