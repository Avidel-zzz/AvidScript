using System;
using System.Collections.Generic;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Text;

namespace AvidScript.CSharpSemantic;

internal static class SemanticAsyncStructuredStatementProjector
{
    public static bool TryProject(
        SemanticCompilationContext context,
        SemanticModel semanticModel,
        StatementSyntax statement,
        SemanticTypeRegistry typeRegistry,
        ICollection<SemanticDiagnostic> diagnostics,
        ref int nodeCount,
        out SemanticAsyncStatement? projected)
    {
        projected = null;
        if (!TryProjectOperation(
            context,
            semanticModel,
            statement,
            typeRegistry,
            diagnostics,
            depth: 0,
            loopDepth: 0,
            ref nodeCount,
            out SemanticOperation? operation))
        {
            return false;
        }

        projected = new SemanticAsyncStatement(operation!, null);
        return true;
    }

    private static bool TryProjectOperation(
        SemanticCompilationContext context,
        SemanticModel semanticModel,
        StatementSyntax statement,
        SemanticTypeRegistry typeRegistry,
        ICollection<SemanticDiagnostic> diagnostics,
        int depth,
        int loopDepth,
        ref int nodeCount,
        out SemanticOperation? projected)
    {
        projected = null;
        if (depth > SemanticAsyncMethod.MaximumStructuredFlowDepth)
        {
            diagnostics.Add(Error(
                "ASCS5416",
                $"Controlled async flow exceeds the nesting-depth limit of {SemanticAsyncMethod.MaximumStructuredFlowDepth}.",
                context,
                statement.Span));
            return false;
        }
        if (++nodeCount > SemanticAsyncMethod.MaximumStructuredFlowNodes)
        {
            diagnostics.Add(Error(
                "ASCS5416",
                $"Controlled async flow exceeds the {SemanticAsyncMethod.MaximumStructuredFlowNodes}-node method limit.",
                context,
                statement.Span));
            return false;
        }

        switch (statement)
        {
            case BlockSyntax block:
                return TryProjectBlock(
                    context,
                    semanticModel,
                    block.Statements,
                    block.Span,
                    typeRegistry,
                    diagnostics,
                    depth,
                    loopDepth,
                    ref nodeCount,
                    out projected);

            case LocalDeclarationStatementSyntax local:
                return TryProjectLocalDeclaration(
                    context,
                    semanticModel,
                    local.Declaration,
                    local.UsingKeyword,
                    local.Span,
                    typeRegistry,
                    diagnostics,
                    depth,
                    loopDepth,
                    ref nodeCount,
                    out projected);

            case ExpressionStatementSyntax expression:
                return TryProjectValue(
                    context,
                    semanticModel,
                    expression,
                    typeRegistry,
                    diagnostics,
                    out projected);

            case IfStatementSyntax conditional:
                return TryProjectIf(
                    context,
                    semanticModel,
                    conditional,
                    typeRegistry,
                    diagnostics,
                    depth,
                    loopDepth,
                    ref nodeCount,
                    out projected);

            case WhileStatementSyntax loop:
                return TryProjectWhile(
                    context,
                    semanticModel,
                    loop,
                    typeRegistry,
                    diagnostics,
                    depth,
                    loopDepth,
                    ref nodeCount,
                    out projected);

            case DoStatementSyntax loop:
                return TryProjectDoWhile(
                    context,
                    semanticModel,
                    loop,
                    typeRegistry,
                    diagnostics,
                    depth,
                    loopDepth,
                    ref nodeCount,
                    out projected);

            case ForStatementSyntax loop:
                return TryProjectFor(
                    context,
                    semanticModel,
                    loop,
                    typeRegistry,
                    diagnostics,
                    depth,
                    loopDepth,
                    ref nodeCount,
                    out projected);

            case BreakStatementSyntax:
                return TryProjectLoopTransfer(
                    context,
                    statement,
                    loopDepth,
                    SemanticAsyncMethod.BreakOperationKind,
                    out projected,
                    diagnostics);

            case ContinueStatementSyntax:
                return TryProjectLoopTransfer(
                    context,
                    statement,
                    loopDepth,
                    SemanticAsyncMethod.ContinueOperationKind,
                    out projected,
                    diagnostics);

            case ReturnStatementSyntax { Expression: null }:
                projected = CreateFlowOperation(
                    SemanticAsyncMethod.ReturnOperationKind,
                    context,
                    statement.Span);
                return true;

            case EmptyStatementSyntax:
                projected = CreateFlowOperation(
                    SemanticAsyncMethod.BlockOperationKind,
                    context,
                    statement.Span);
                return true;

            default:
                diagnostics.Add(Error(
                    "ASCS5415",
                    "Controlled async flow supports blocks, local declarations, expressions, if/else, for/while/do, break, continue, and void return statements.",
                    context,
                    statement.Span));
                return false;
        }
    }

    private static bool TryProjectBlock(
        SemanticCompilationContext context,
        SemanticModel semanticModel,
        SyntaxList<StatementSyntax> statements,
        TextSpan span,
        SemanticTypeRegistry typeRegistry,
        ICollection<SemanticDiagnostic> diagnostics,
        int depth,
        int loopDepth,
        ref int nodeCount,
        out SemanticOperation? projected)
    {
        List<SemanticOperation> children = new(statements.Count);
        foreach (StatementSyntax statement in statements)
        {
            if (!TryProjectOperation(
                context,
                semanticModel,
                statement,
                typeRegistry,
                diagnostics,
                depth + 1,
                loopDepth,
                ref nodeCount,
                out SemanticOperation? child))
            {
                projected = null;
                return false;
            }
            children.Add(child!);
        }

        projected = CreateFlowOperation(
            SemanticAsyncMethod.BlockOperationKind,
            context,
            span,
            children: children);
        return true;
    }

    private static bool TryProjectLocalDeclaration(
        SemanticCompilationContext context,
        SemanticModel semanticModel,
        VariableDeclarationSyntax declaration,
        SyntaxToken usingKeyword,
        TextSpan statementSpan,
        SemanticTypeRegistry typeRegistry,
        ICollection<SemanticDiagnostic> diagnostics,
        int depth,
        int loopDepth,
        ref int nodeCount,
        out SemanticOperation? projected)
    {
        if (!usingKeyword.IsKind(Microsoft.CodeAnalysis.CSharp.SyntaxKind.None))
        {
            diagnostics.Add(Error(
                "ASCS5415",
                "Controlled async local declarations do not support using ownership.",
                context,
                statementSpan));
            projected = null;
            return false;
        }

        List<SemanticOperation> declarations = new(declaration.Variables.Count);
        foreach (VariableDeclaratorSyntax variable in declaration.Variables)
        {
            if (semanticModel.GetDeclaredSymbol(variable) is not ILocalSymbol symbol)
            {
                diagnostics.Add(Error(
                    "ASCS5415",
                    "Roslyn did not expose a stable symbol for the async local declaration.",
                    context,
                    variable.Span));
                projected = null;
                return false;
            }

            List<SemanticOperation> initializer = new(1);
            if (variable.Initializer is not null)
            {
                if (!TryProjectValue(
                    context,
                    semanticModel,
                    variable.Initializer.Value,
                    typeRegistry,
                    diagnostics,
                    out SemanticOperation? value))
                {
                    projected = null;
                    return false;
                }
                initializer.Add(value!);
            }

            declarations.Add(CreateFlowOperation(
                SemanticAsyncMethod.LocalDeclarationOperationKind,
                context,
                variable.Span,
                typeId: typeRegistry.Register(symbol.Type),
                symbolId: SemanticSymbolProjector.GetSymbolId(symbol),
                children: initializer));
        }

        if (declarations.Count == 1)
        {
            projected = declarations[0];
            return true;
        }

        projected = CreateFlowOperation(
            SemanticAsyncMethod.BlockOperationKind,
            context,
            statementSpan,
            children: declarations);
        return true;
    }

    private static bool TryProjectIf(
        SemanticCompilationContext context,
        SemanticModel semanticModel,
        IfStatementSyntax conditional,
        SemanticTypeRegistry typeRegistry,
        ICollection<SemanticDiagnostic> diagnostics,
        int depth,
        int loopDepth,
        ref int nodeCount,
        out SemanticOperation? projected)
    {
        if (!TryProjectBool(
                context,
                semanticModel,
                conditional.Condition,
                typeRegistry,
                diagnostics,
                out SemanticOperation? condition)
            || !TryProjectAsBlock(
                context,
                semanticModel,
                conditional.Statement,
                typeRegistry,
                diagnostics,
                depth,
                loopDepth,
                ref nodeCount,
                out SemanticOperation? whenTrue))
        {
            projected = null;
            return false;
        }

        List<SemanticOperation> children = new() { condition!, whenTrue! };
        if (conditional.Else is not null)
        {
            if (!TryProjectAsBlock(
                context,
                semanticModel,
                conditional.Else.Statement,
                typeRegistry,
                diagnostics,
                depth,
                loopDepth,
                ref nodeCount,
                out SemanticOperation? whenFalse))
            {
                projected = null;
                return false;
            }
            children.Add(whenFalse!);
        }

        projected = CreateFlowOperation(
            SemanticAsyncMethod.IfOperationKind,
            context,
            conditional.Span,
            children: children);
        return true;
    }

    private static bool TryProjectWhile(
        SemanticCompilationContext context,
        SemanticModel semanticModel,
        WhileStatementSyntax loop,
        SemanticTypeRegistry typeRegistry,
        ICollection<SemanticDiagnostic> diagnostics,
        int depth,
        int loopDepth,
        ref int nodeCount,
        out SemanticOperation? projected)
    {
        if (!TryProjectBool(context, semanticModel, loop.Condition, typeRegistry, diagnostics, out SemanticOperation? condition)
            || !TryProjectAsBlock(context, semanticModel, loop.Statement, typeRegistry, diagnostics, depth, loopDepth + 1, ref nodeCount, out SemanticOperation? body))
        {
            projected = null;
            return false;
        }

        projected = CreateFlowOperation(
            SemanticAsyncMethod.WhileOperationKind,
            context,
            loop.Span,
            children: new[] { condition!, body! });
        return true;
    }

    private static bool TryProjectDoWhile(
        SemanticCompilationContext context,
        SemanticModel semanticModel,
        DoStatementSyntax loop,
        SemanticTypeRegistry typeRegistry,
        ICollection<SemanticDiagnostic> diagnostics,
        int depth,
        int loopDepth,
        ref int nodeCount,
        out SemanticOperation? projected)
    {
        if (!TryProjectAsBlock(context, semanticModel, loop.Statement, typeRegistry, diagnostics, depth, loopDepth + 1, ref nodeCount, out SemanticOperation? body)
            || !TryProjectBool(context, semanticModel, loop.Condition, typeRegistry, diagnostics, out SemanticOperation? condition))
        {
            projected = null;
            return false;
        }

        projected = CreateFlowOperation(
            SemanticAsyncMethod.DoWhileOperationKind,
            context,
            loop.Span,
            children: new[] { body!, condition! });
        return true;
    }

    private static bool TryProjectFor(
        SemanticCompilationContext context,
        SemanticModel semanticModel,
        ForStatementSyntax loop,
        SemanticTypeRegistry typeRegistry,
        ICollection<SemanticDiagnostic> diagnostics,
        int depth,
        int loopDepth,
        ref int nodeCount,
        out SemanticOperation? projected)
    {
        List<SemanticOperation> initializers = new();
        if (loop.Declaration is not null)
        {
            if (!TryProjectLocalDeclaration(
                context,
                semanticModel,
                loop.Declaration,
                default,
                loop.Declaration.Span,
                typeRegistry,
                diagnostics,
                depth,
                loopDepth,
                ref nodeCount,
                out SemanticOperation? initializer))
            {
                projected = null;
                return false;
            }
            initializers.Add(initializer!);
        }
        foreach (ExpressionSyntax initializer in loop.Initializers)
        {
            if (!TryProjectValue(context, semanticModel, initializer, typeRegistry, diagnostics, out SemanticOperation? value))
            {
                projected = null;
                return false;
            }
            initializers.Add(value!);
        }

        SemanticOperation condition;
        if (loop.Condition is null)
        {
            condition = CreateTrueLiteral(context, loop.ForKeyword.Span);
        }
        else if (!TryProjectBool(context, semanticModel, loop.Condition, typeRegistry, diagnostics, out SemanticOperation? projectedCondition))
        {
            projected = null;
            return false;
        }
        else
        {
            condition = projectedCondition!;
        }

        List<SemanticOperation> increments = new(loop.Incrementors.Count);
        foreach (ExpressionSyntax increment in loop.Incrementors)
        {
            if (!TryProjectValue(context, semanticModel, increment, typeRegistry, diagnostics, out SemanticOperation? value))
            {
                projected = null;
                return false;
            }
            increments.Add(value!);
        }
        if (!TryProjectAsBlock(context, semanticModel, loop.Statement, typeRegistry, diagnostics, depth, loopDepth + 1, ref nodeCount, out SemanticOperation? body))
        {
            projected = null;
            return false;
        }

        projected = CreateFlowOperation(
            SemanticAsyncMethod.ForOperationKind,
            context,
            loop.Span,
            children: new[]
            {
                CreateFlowOperation(SemanticAsyncMethod.BlockOperationKind, context, loop.Span, children: initializers),
                condition,
                CreateFlowOperation(SemanticAsyncMethod.BlockOperationKind, context, loop.Span, children: increments),
                body!,
            });
        return true;
    }

    private static bool TryProjectAsBlock(
        SemanticCompilationContext context,
        SemanticModel semanticModel,
        StatementSyntax statement,
        SemanticTypeRegistry typeRegistry,
        ICollection<SemanticDiagnostic> diagnostics,
        int depth,
        int loopDepth,
        ref int nodeCount,
        out SemanticOperation? projected)
    {
        if (!TryProjectOperation(context, semanticModel, statement, typeRegistry, diagnostics, depth + 1, loopDepth, ref nodeCount, out SemanticOperation? child))
        {
            projected = null;
            return false;
        }
        projected = child!.Kind == SemanticAsyncMethod.BlockOperationKind
            ? child
            : CreateFlowOperation(
                SemanticAsyncMethod.BlockOperationKind,
                context,
                statement.Span,
                children: new[] { child });
        return true;
    }

    private static bool TryProjectBool(
        SemanticCompilationContext context,
        SemanticModel semanticModel,
        ExpressionSyntax expression,
        SemanticTypeRegistry typeRegistry,
        ICollection<SemanticDiagnostic> diagnostics,
        out SemanticOperation? projected)
    {
        if (!TryProjectValue(context, semanticModel, expression, typeRegistry, diagnostics, out projected))
        {
            return false;
        }
        if (projected!.TypeId == "type:bool")
        {
            return true;
        }

        diagnostics.Add(Error(
            "ASCS5415",
            "Controlled async branch and loop conditions must be supported bool expressions.",
            context,
            expression.Span));
        projected = null;
        return false;
    }

    private static bool TryProjectValue(
        SemanticCompilationContext context,
        SemanticModel semanticModel,
        SyntaxNode syntax,
        SemanticTypeRegistry typeRegistry,
        ICollection<SemanticDiagnostic> diagnostics,
        out SemanticOperation? projected)
    {
        IOperation? operation = semanticModel.GetOperation(syntax);
        if (operation is null || SemanticAsyncProjector.ContainsAsyncHelperOrTask(operation))
        {
            diagnostics.Add(Error(
                "ASCS5403",
                "Structured async flow requires a supported synchronous Roslyn operation.",
                context,
                syntax.Span));
            projected = null;
            return false;
        }

        int before = diagnostics.Count;
        projected = SemanticOperationProjector.ProjectAsyncStatementOperation(
            operation,
            context.PrimaryUnit,
            typeRegistry,
            diagnostics);
        if (diagnostics.Count == before && SemanticAsyncProjector.AllOperationsSupported(projected))
        {
            return true;
        }
        projected = null;
        return false;
    }

    private static bool TryProjectLoopTransfer(
        SemanticCompilationContext context,
        StatementSyntax statement,
        int loopDepth,
        string kind,
        out SemanticOperation? projected,
        ICollection<SemanticDiagnostic> diagnostics)
    {
        if (loopDepth <= 0)
        {
            diagnostics.Add(Error(
                "ASCS5415",
                "Controlled async break and continue statements require a containing loop.",
                context,
                statement.Span));
            projected = null;
            return false;
        }
        projected = CreateFlowOperation(kind, context, statement.Span);
        return true;
    }

    private static SemanticOperation CreateTrueLiteral(
        SemanticCompilationContext context,
        TextSpan span)
    {
        return new SemanticOperation(
            "literal",
            true,
            null,
            false,
            false,
            false,
            false,
            "type:bool",
            null,
            Array.Empty<string>(),
            new SemanticConstant("bool", "true"),
            null,
            null,
            null,
            null,
            SemanticSpanFactory.Create(context.PrimaryUnit.SourceText, span),
            Array.Empty<SemanticOperation>());
    }

    private static SemanticOperation CreateFlowOperation(
        string kind,
        SemanticCompilationContext context,
        TextSpan span,
        string? typeId = "type:void",
        string? symbolId = null,
        IReadOnlyList<SemanticOperation>? children = null)
    {
        return new SemanticOperation(
            kind,
            true,
            null,
            false,
            false,
            false,
            false,
            typeId,
            symbolId,
            Array.Empty<string>(),
            null,
            null,
            null,
            null,
            null,
            SemanticSpanFactory.Create(context.PrimaryUnit.SourceText, span),
            children ?? Array.Empty<SemanticOperation>());
    }

    private static SemanticDiagnostic Error(
        string code,
        string message,
        SemanticCompilationContext context,
        TextSpan span)
    {
        return new SemanticDiagnostic(
            code,
            "error",
            message,
            SemanticSpanFactory.Create(context.PrimaryUnit.SourceText, span));
    }
}
