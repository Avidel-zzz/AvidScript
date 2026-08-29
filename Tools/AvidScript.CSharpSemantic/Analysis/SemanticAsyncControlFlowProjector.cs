using System;
using System.Collections.Generic;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Operations;
using Microsoft.CodeAnalysis.Text;

namespace AvidScript.CSharpSemantic;

internal sealed record SemanticAsyncControlFlowProjection(
    IReadOnlyList<SemanticAsyncSegment> Segments,
    int EntrySegmentOrdinal,
    IReadOnlyList<SemanticAsyncCompilerLocal> CompilerLocals);

internal static class SemanticAsyncControlFlowProjector
{
    public static bool TryProject(
        SemanticCompilationContext context,
        SemanticModel semanticModel,
        BlockSyntax body,
        string methodSymbolId,
        SemanticTypeRegistry typeRegistry,
        ICollection<SemanticDiagnostic> diagnostics,
        ref int nextCallbackId,
        out SemanticAsyncControlFlowProjection? projected)
    {
        Builder builder = new(
            context,
            semanticModel,
            methodSymbolId,
            typeRegistry,
            diagnostics);
        if (!builder.TryBuild(body, ref nextCallbackId, out projected))
        {
            projected = null;
            return false;
        }
        return true;
    }

    private sealed class Builder
    {
        private readonly SemanticCompilationContext context;
        private readonly SemanticModel semanticModel;
        private readonly string methodSymbolId;
        private readonly SemanticTypeRegistry typeRegistry;
        private readonly ICollection<SemanticDiagnostic> diagnostics;
        private readonly List<DraftSegment> drafts = new();
        private readonly List<SemanticAsyncCompilerLocal> compilerLocals = new();
        private int structuredNodeCount;
        private bool failed;

        public Builder(
            SemanticCompilationContext context,
            SemanticModel semanticModel,
            string methodSymbolId,
            SemanticTypeRegistry typeRegistry,
            ICollection<SemanticDiagnostic> diagnostics)
        {
            this.context = context;
            this.semanticModel = semanticModel;
            this.methodSymbolId = methodSymbolId;
            this.typeRegistry = typeRegistry;
            this.diagnostics = diagnostics;
        }

        public bool TryBuild(
            BlockSyntax body,
            ref int nextCallbackId,
            out SemanticAsyncControlFlowProjection? projected)
        {
            int exit = AddDraft(
                body.CloseBraceToken.Span,
                Array.Empty<SemanticAsyncStatement>(),
                null,
                new DraftTransfer(SemanticAsyncMethod.ReturnTransferKind, null, -1, -1));
            int entry = BuildSequence(
                body.Statements,
                exit,
                LoopTargets.None,
                depth: 0);
            if (failed || entry < 0)
            {
                projected = null;
                return false;
            }

            HashSet<int> reachable = CollectReachable(entry);
            DraftSegment[] ordered = drafts
                .Where(draft => reachable.Contains(draft.Id))
                .OrderBy(draft => draft.Span.Start)
                .ThenBy(draft => draft.Sequence)
                .ToArray();
            if (ordered.Length > SemanticAsyncMethod.MaximumControlFlowSegments)
            {
                diagnostics.Add(Error(
                    "ASCS5417",
                    $"Controlled async CFG exceeds the {SemanticAsyncMethod.MaximumControlFlowSegments}-segment method limit.",
                    body.Span));
                projected = null;
                return false;
            }

            Dictionary<int, int> ordinalByDraft = ordered
                .Select((draft, ordinal) => (draft.Id, ordinal))
                .ToDictionary(pair => pair.Id, pair => pair.ordinal);
            Dictionary<int, int> callbackByDraft = new();
            foreach (DraftSegment draft in ordered
                .Where(item => item.AwaitSite is not null)
                .OrderBy(item => item.AwaitSite!.Span.Start)
                .ThenBy(item => item.Sequence))
            {
                callbackByDraft.Add(draft.Id, nextCallbackId++);
            }

            List<SemanticAsyncSegment> segments = new(ordered.Length);
            for (int ordinal = 0; ordinal < ordered.Length; ++ordinal)
            {
                DraftSegment draft = ordered[ordinal];
                SemanticAsyncAwaitSite? awaitSite = draft.AwaitSite is null
                    ? null
                    : draft.AwaitSite with { CallbackId = callbackByDraft[draft.Id] };
                SemanticAsyncControlTransfer transfer = new(
                    draft.Transfer.Kind,
                    draft.Transfer.Condition,
                    RemapTarget(draft.Transfer.PrimaryTarget, ordinalByDraft),
                    RemapTarget(draft.Transfer.SecondaryTarget, ordinalByDraft));
                segments.Add(new SemanticAsyncSegment(
                    ordinal,
                    draft.Statements,
                    awaitSite,
                    SemanticSpanFactory.Create(context.PrimaryUnit.SourceText, draft.Span),
                    transfer));
            }

            projected = new SemanticAsyncControlFlowProjection(
                segments,
                ordinalByDraft[entry],
                compilerLocals
                    .OrderBy(local => local.SymbolId, StringComparer.Ordinal)
                    .ToArray());
            return true;
        }

        private int BuildSequence(
            SyntaxList<StatementSyntax> statements,
            int successor,
            LoopTargets targets,
            int depth)
        {
            int entry = successor;
            for (int index = statements.Count - 1; index >= 0; --index)
            {
                entry = BuildStatement(statements[index], entry, targets, depth);
                if (entry < 0)
                {
                    break;
                }
            }
            return entry;
        }

        private int BuildStatement(
            StatementSyntax statement,
            int successor,
            LoopTargets targets,
            int depth)
        {
            if (failed)
            {
                return -1;
            }
            if (depth > SemanticAsyncMethod.MaximumStructuredFlowDepth)
            {
                diagnostics.Add(Error(
                    "ASCS5416",
                    $"Controlled async flow exceeds the nesting-depth limit of {SemanticAsyncMethod.MaximumStructuredFlowDepth}.",
                    statement.Span));
                failed = true;
                return -1;
            }

            if (SemanticAsyncProjector.TryGetDirectAwait(
                statement,
                out AwaitExpressionSyntax? awaitExpression,
                out VariableDeclaratorSyntax? result))
            {
                if (!SemanticAsyncProjector.TryProjectAwaitSite(
                    context,
                    semanticModel,
                    awaitExpression!,
                    result,
                    typeRegistry,
                    diagnostics,
                    callbackId: -1,
                    out SemanticAsyncAwaitSite? awaitSite))
                {
                    failed = true;
                    return -1;
                }
                return AddDraft(
                    statement.Span,
                    Array.Empty<SemanticAsyncStatement>(),
                    awaitSite,
                    new DraftTransfer(
                        SemanticAsyncMethod.AwaitTransferKind,
                        null,
                        successor,
                        -1));
            }

            switch (statement)
            {
                case BlockSyntax block:
                    return BuildSequence(block.Statements, successor, targets, depth + 1);

                case IfStatementSyntax conditional:
                    return BuildIf(conditional, successor, targets, depth);

                case WhileStatementSyntax loop:
                    return BuildWhile(loop, successor, targets, depth);

                case DoStatementSyntax loop:
                    return BuildDoWhile(loop, successor, targets, depth);

                case ForStatementSyntax loop:
                    return BuildFor(loop, successor, targets, depth);

                case ForEachStatementSyntax loop:
                    return BuildForEach(loop, successor, targets, depth);

                case ForEachVariableStatementSyntax loop:
                    return Reject(
                        "Controlled async foreach does not support deconstruction variables.",
                        loop.Span,
                        "ASCS5419");

                case SwitchStatementSyntax switchStatement:
                    return BuildSwitch(switchStatement, successor, targets, depth);

                case BreakStatementSyntax:
                    if (targets.BreakTarget < 0)
                    {
                        return Reject(
                            "Controlled async break requires a containing loop.",
                            statement.Span);
                    }
                    return AddGoto(statement.Span, targets.BreakTarget);

                case ContinueStatementSyntax:
                    if (targets.ContinueTarget < 0)
                    {
                        return Reject(
                            "Controlled async continue requires a containing loop.",
                            statement.Span);
                    }
                    return AddGoto(statement.Span, targets.ContinueTarget);

                case ReturnStatementSyntax { Expression: null }:
                    return AddDraft(
                        statement.Span,
                        Array.Empty<SemanticAsyncStatement>(),
                        null,
                        new DraftTransfer(
                            SemanticAsyncMethod.ReturnTransferKind,
                            null,
                            -1,
                            -1));

                case GotoStatementSyntax gotoStatement:
                    return Reject(
                        "Controlled async switch does not support goto case, goto default, or labels.",
                        gotoStatement.Span,
                        "ASCS5418");

                case EmptyStatementSyntax:
                    return AddGoto(statement.Span, successor);
            }

            if (statement.DescendantNodesAndSelf().OfType<AwaitExpressionSyntax>().Any())
            {
                return Reject(
                    "Await expressions must be direct statements or single-local initializers inside supported async control flow.",
                    statement.Span,
                    "ASCS5402");
            }
            if (!SemanticAsyncProjector.TryProjectStatement(
                context,
                semanticModel,
                statement,
                typeRegistry,
                diagnostics,
                ref structuredNodeCount,
                out SemanticAsyncStatement? projectedStatement))
            {
                failed = true;
                return -1;
            }
            return AddDraft(
                statement.Span,
                new[] { projectedStatement! },
                null,
                new DraftTransfer(
                    SemanticAsyncMethod.GotoTransferKind,
                    null,
                    successor,
                    -1));
        }

        private int BuildIf(
            IfStatementSyntax conditional,
            int successor,
            LoopTargets targets,
            int depth)
        {
            if (!TryProjectBool(condition: conditional.Condition, out SemanticOperation? condition))
            {
                return -1;
            }
            int whenTrue = BuildStatement(
                conditional.Statement,
                successor,
                targets,
                depth + 1);
            if (whenTrue < 0)
            {
                return -1;
            }
            int whenFalse = conditional.Else is null
                ? successor
                : BuildStatement(
                    conditional.Else.Statement,
                    successor,
                    targets,
                    depth + 1);
            if (whenFalse < 0)
            {
                return -1;
            }
            return AddDraft(
                conditional.Condition.Span,
                Array.Empty<SemanticAsyncStatement>(),
                null,
                new DraftTransfer(
                    SemanticAsyncMethod.BranchTransferKind,
                    condition,
                    whenTrue,
                    whenFalse));
        }

        private int BuildWhile(
            WhileStatementSyntax loop,
            int successor,
            LoopTargets outerTargets,
            int depth)
        {
            if (!TryProjectBool(loop.Condition, out SemanticOperation? condition))
            {
                return -1;
            }
            int conditionDraft = AddDraft(
                loop.Condition.Span,
                Array.Empty<SemanticAsyncStatement>(),
                null,
                new DraftTransfer(SemanticAsyncMethod.ReturnTransferKind, null, -1, -1));
            if (conditionDraft < 0)
            {
                return -1;
            }
            int body = BuildStatement(
                loop.Statement,
                conditionDraft,
                new LoopTargets(successor, conditionDraft),
                depth + 1);
            drafts[conditionDraft].Transfer = new DraftTransfer(
                SemanticAsyncMethod.BranchTransferKind,
                condition,
                body,
                successor);
            return conditionDraft;
        }

        private int BuildDoWhile(
            DoStatementSyntax loop,
            int successor,
            LoopTargets outerTargets,
            int depth)
        {
            if (!TryProjectBool(loop.Condition, out SemanticOperation? condition))
            {
                return -1;
            }
            int conditionDraft = AddDraft(
                loop.Condition.Span,
                Array.Empty<SemanticAsyncStatement>(),
                null,
                new DraftTransfer(SemanticAsyncMethod.ReturnTransferKind, null, -1, -1));
            if (conditionDraft < 0)
            {
                return -1;
            }
            int body = BuildStatement(
                loop.Statement,
                conditionDraft,
                new LoopTargets(successor, conditionDraft),
                depth + 1);
            drafts[conditionDraft].Transfer = new DraftTransfer(
                SemanticAsyncMethod.BranchTransferKind,
                condition,
                body,
                successor);
            return body;
        }

        private int BuildFor(
            ForStatementSyntax loop,
            int successor,
            LoopTargets outerTargets,
            int depth)
        {
            IEnumerable<SyntaxNode> headerNodes = loop.Initializers.Cast<SyntaxNode>()
                .Concat(loop.Incrementors)
                .Append(loop.Condition)
                .Where(node => node is not null)!;
            if (loop.Declaration?.DescendantNodesAndSelf().OfType<AwaitExpressionSyntax>().Any() == true
                || headerNodes.Any(node => node.DescendantNodesAndSelf().OfType<AwaitExpressionSyntax>().Any()))
            {
                return Reject(
                    "Controlled async for initializers, conditions, and incrementors must remain synchronous.",
                    loop.Span,
                    "ASCS5402");
            }

            SemanticOperation condition;
            if (loop.Condition is null)
            {
                condition = CreateTrueLiteral(loop.ForKeyword.Span);
            }
            else if (!TryProjectBool(loop.Condition, out SemanticOperation? projectedCondition))
            {
                return -1;
            }
            else
            {
                condition = projectedCondition!;
            }

            int conditionDraft = AddDraft(
                loop.Condition?.Span ?? loop.ForKeyword.Span,
                Array.Empty<SemanticAsyncStatement>(),
                null,
                new DraftTransfer(SemanticAsyncMethod.ReturnTransferKind, null, -1, -1));
            if (conditionDraft < 0)
            {
                return -1;
            }
            int incrementEntry = conditionDraft;
            for (int index = loop.Incrementors.Count - 1; index >= 0; --index)
            {
                if (!TryProjectValueStatement(loop.Incrementors[index], out SemanticAsyncStatement? increment))
                {
                    return -1;
                }
                incrementEntry = AddDraft(
                    loop.Incrementors[index].Span,
                    new[] { increment! },
                    null,
                    new DraftTransfer(
                        SemanticAsyncMethod.GotoTransferKind,
                        null,
                        incrementEntry,
                        -1));
                if (incrementEntry < 0)
                {
                    return -1;
                }
            }

            int body = BuildStatement(
                loop.Statement,
                incrementEntry,
                new LoopTargets(successor, incrementEntry),
                depth + 1);
            drafts[conditionDraft].Transfer = new DraftTransfer(
                SemanticAsyncMethod.BranchTransferKind,
                condition,
                body,
                successor);

            int entry = conditionDraft;
            for (int index = loop.Initializers.Count - 1; index >= 0; --index)
            {
                if (!TryProjectValueStatement(loop.Initializers[index], out SemanticAsyncStatement? initializer))
                {
                    return -1;
                }
                entry = AddDraft(
                    loop.Initializers[index].Span,
                    new[] { initializer! },
                    null,
                    new DraftTransfer(
                        SemanticAsyncMethod.GotoTransferKind,
                        null,
                        entry,
                        -1));
                if (entry < 0)
                {
                    return -1;
                }
            }
            if (loop.Declaration is not null)
            {
                if (!TryProjectForDeclaration(loop.Declaration, out SemanticAsyncStatement? declaration))
                {
                    return -1;
                }
                entry = AddDraft(
                    loop.Declaration.Span,
                    new[] { declaration! },
                    null,
                    new DraftTransfer(
                        SemanticAsyncMethod.GotoTransferKind,
                        null,
                        entry,
                        -1));
                if (entry < 0)
                {
                    return -1;
                }
            }
            return entry;
        }

        private int BuildForEach(
            ForEachStatementSyntax loop,
            int successor,
            LoopTargets outerTargets,
            int depth)
        {
            if (!loop.AwaitKeyword.IsKind(Microsoft.CodeAnalysis.CSharp.SyntaxKind.None)
                || loop.Type is RefTypeSyntax
                || loop.Expression.DescendantNodesAndSelf().OfType<AwaitExpressionSyntax>().Any())
            {
                return Reject(
                    "Controlled async foreach requires a synchronous value iterator over a one-dimensional array.",
                    loop.Span,
                    "ASCS5419");
            }

            ITypeSymbol? collectionType = semanticModel.GetTypeInfo(loop.Expression).Type;
            if (collectionType is not IArrayTypeSymbol { Rank: 1 } arrayType
                || semanticModel.GetDeclaredSymbol(loop) is not ILocalSymbol itemSymbol
                || !SymbolEqualityComparer.Default.Equals(itemSymbol.Type, arrayType.ElementType))
            {
                return Reject(
                    "Controlled async foreach supports one-dimensional arrays with an exact value element type only.",
                    loop.Span,
                    "ASCS5419");
            }
            if (!TryProjectValue(loop.Expression, out SemanticOperation? collection))
            {
                return -1;
            }

            string arrayTypeId = typeRegistry.Register(arrayType);
            string elementTypeId = typeRegistry.Register(arrayType.ElementType);
            string itemSymbolId = SemanticSymbolProjector.GetSymbolId(itemSymbol);
            string localPrefix = $"symbol:compiler_local:{methodSymbolId}:foreach:{loop.SpanStart}";
            string arraySymbolId = localPrefix + ":array";
            string indexSymbolId = localPrefix + ":index";
            SemanticSpan loopSpan = SemanticSpanFactory.Create(
                context.PrimaryUnit.SourceText,
                loop.Span);
            compilerLocals.Add(new SemanticAsyncCompilerLocal(
                arraySymbolId,
                $"<foreach_array_{loop.SpanStart}>",
                arrayTypeId,
                loopSpan));
            compilerLocals.Add(new SemanticAsyncCompilerLocal(
                indexSymbolId,
                $"<foreach_index_{loop.SpanStart}>",
                "type:int32",
                loopSpan));

            SemanticOperation arrayReference = CreateValueOperation(
                "local_reference",
                arrayTypeId,
                arraySymbolId,
                loop.Expression.Span);
            SemanticOperation indexReference = CreateValueOperation(
                "local_reference",
                "type:int32",
                indexSymbolId,
                loop.Identifier.Span);
            SemanticOperation length = CreateValueOperation(
                "property_reference",
                "type:int32",
                SemanticIntrinsicIds.ArrayLengthPropertyId,
                loop.Expression.Span,
                new[] { arrayReference });
            SemanticOperation condition = CreateValueOperation(
                "binary",
                "type:bool",
                null,
                loop.Expression.Span,
                new[] { indexReference, length },
                operatorKind: "less_than");

            int conditionDraft = AddDraft(
                loop.Expression.Span,
                Array.Empty<SemanticAsyncStatement>(),
                null,
                new DraftTransfer(SemanticAsyncMethod.ReturnTransferKind, null, -1, -1));
            if (conditionDraft < 0)
            {
                return -1;
            }

            SemanticOperation increment = CreateValueOperation(
                "increment_or_decrement",
                "type:int32",
                null,
                loop.Identifier.Span,
                new[] { indexReference },
                operatorKind: "increment");
            int incrementDraft = AddDraft(
                loop.Identifier.Span,
                new[] { new SemanticAsyncStatement(increment, null) },
                null,
                new DraftTransfer(
                    SemanticAsyncMethod.GotoTransferKind,
                    null,
                    conditionDraft,
                    -1));
            if (incrementDraft < 0)
            {
                return -1;
            }

            int body = BuildStatement(
                loop.Statement,
                incrementDraft,
                new LoopTargets(successor, incrementDraft),
                depth + 1);
            if (body < 0)
            {
                return -1;
            }
            SemanticOperation element = CreateValueOperation(
                "array_element_reference",
                elementTypeId,
                null,
                loop.Identifier.Span,
                new[] { arrayReference, indexReference });
            int itemDraft = AddDraft(
                loop.Identifier.Span,
                new[] { new SemanticAsyncStatement(element, itemSymbolId) },
                null,
                new DraftTransfer(SemanticAsyncMethod.GotoTransferKind, null, body, -1));
            if (itemDraft < 0)
            {
                return -1;
            }
            drafts[conditionDraft].Transfer = new DraftTransfer(
                SemanticAsyncMethod.BranchTransferKind,
                condition,
                itemDraft,
                successor);

            SemanticOperation zero = CreateValueOperation(
                "literal",
                "type:int32",
                null,
                loop.ForEachKeyword.Span,
                constant: new SemanticConstant("int32", "0"));
            int indexDraft = AddDraft(
                loop.ForEachKeyword.Span,
                new[] { new SemanticAsyncStatement(zero, indexSymbolId) },
                null,
                new DraftTransfer(SemanticAsyncMethod.GotoTransferKind, null, conditionDraft, -1));
            if (indexDraft < 0)
            {
                return -1;
            }
            return AddDraft(
                loop.ForEachKeyword.Span,
                new[] { new SemanticAsyncStatement(collection!, arraySymbolId) },
                null,
                new DraftTransfer(SemanticAsyncMethod.GotoTransferKind, null, indexDraft, -1));
        }

        private int BuildSwitch(
            SwitchStatementSyntax switchStatement,
            int successor,
            LoopTargets outerTargets,
            int depth)
        {
            if (!TryProjectSwitchValue(
                switchStatement.Expression,
                out SemanticOperation? governingValue))
            {
                return -1;
            }

            List<SwitchCaseTarget> cases = new();
            int defaultTarget = successor;
            bool hasDefault = false;
            foreach (SwitchSectionSyntax section in switchStatement.Sections)
            {
                int sectionEntry = BuildSequence(
                    section.Statements,
                    successor,
                    new LoopTargets(successor, outerTargets.ContinueTarget),
                    depth + 1);
                if (sectionEntry < 0)
                {
                    return -1;
                }

                foreach (SwitchLabelSyntax label in section.Labels)
                {
                    switch (label)
                    {
                        case CaseSwitchLabelSyntax caseLabel:
                            if (!semanticModel.GetConstantValue(caseLabel.Value).HasValue
                                || !TryProjectValue(caseLabel.Value, out SemanticOperation? caseValue)
                                || !string.Equals(
                                    governingValue!.TypeId,
                                    caseValue!.TypeId,
                                    StringComparison.Ordinal))
                            {
                                return Reject(
                                    "Controlled async switch case labels must be compatible compile-time constants.",
                                    caseLabel.Span,
                                    "ASCS5418");
                            }
                            cases.Add(new SwitchCaseTarget(
                                caseLabel,
                                caseValue!,
                                sectionEntry));
                            break;

                        case DefaultSwitchLabelSyntax defaultLabel:
                            if (hasDefault)
                            {
                                return Reject(
                                    "Controlled async switch permits one default label.",
                                    defaultLabel.Span,
                                    "ASCS5418");
                            }
                            hasDefault = true;
                            defaultTarget = sectionEntry;
                            break;

                        default:
                            return Reject(
                                "Controlled async switch supports constant case labels and default only.",
                                label.Span,
                                "ASCS5418");
                    }
                }
            }

            int dispatchEntry = defaultTarget;
            for (int index = cases.Count - 1; index >= 0; --index)
            {
                SwitchCaseTarget item = cases[index];
                SemanticOperation condition = CreateEquality(
                    governingValue!,
                    item.Value,
                    item.Label.Span);
                dispatchEntry = AddDraft(
                    item.Label.Span,
                    Array.Empty<SemanticAsyncStatement>(),
                    null,
                    new DraftTransfer(
                        SemanticAsyncMethod.BranchTransferKind,
                        condition,
                        item.SectionTarget,
                        dispatchEntry));
                if (dispatchEntry < 0)
                {
                    return -1;
                }
            }
            return dispatchEntry;
        }

        private bool TryProjectSwitchValue(
            ExpressionSyntax expression,
            out SemanticOperation? projected)
        {
            IOperation? operation = semanticModel.GetOperation(expression);
            ITypeSymbol? type = semanticModel.GetTypeInfo(expression).Type;
            if (!IsStableSwitchRead(operation) || !IsSupportedSwitchType(type))
            {
                diagnostics.Add(Error(
                    "ASCS5418",
                    "Controlled async switch requires an integral or enum local/parameter governing value.",
                    expression.Span));
                projected = null;
                failed = true;
                return false;
            }
            return TryProjectValue(expression, out projected);
        }

        private static bool IsStableSwitchRead(IOperation? operation)
        {
            return operation switch
            {
                ILocalReferenceOperation => true,
                IParameterReferenceOperation => true,
                IConversionOperation conversion => IsStableSwitchRead(conversion.Operand),
                IParenthesizedOperation parenthesized => IsStableSwitchRead(parenthesized.Operand),
                _ => false,
            };
        }

        private static bool IsSupportedSwitchType(ITypeSymbol? type)
        {
            if (type?.TypeKind == TypeKind.Enum)
            {
                return true;
            }
            return type?.SpecialType is
                SpecialType.System_SByte or
                SpecialType.System_Byte or
                SpecialType.System_Int16 or
                SpecialType.System_UInt16 or
                SpecialType.System_Int32 or
                SpecialType.System_UInt32 or
                SpecialType.System_Int64 or
                SpecialType.System_UInt64 or
                SpecialType.System_Char;
        }

        private bool TryProjectForDeclaration(
            VariableDeclarationSyntax declaration,
            out SemanticAsyncStatement? projected)
        {
            List<SemanticOperation> declarations = new(declaration.Variables.Count);
            foreach (VariableDeclaratorSyntax variable in declaration.Variables)
            {
                if (semanticModel.GetDeclaredSymbol(variable) is not ILocalSymbol symbol)
                {
                    diagnostics.Add(Error(
                        "ASCS5415",
                        "Roslyn did not expose a stable symbol for the async for local.",
                        variable.Span));
                    projected = null;
                    failed = true;
                    return false;
                }

                List<SemanticOperation> children = new(1);
                if (variable.Initializer is not null)
                {
                    if (!TryProjectValue(variable.Initializer.Value, out SemanticOperation? value))
                    {
                        projected = null;
                        return false;
                    }
                    children.Add(value!);
                }
                declarations.Add(CreateFlowOperation(
                    SemanticAsyncMethod.LocalDeclarationOperationKind,
                    variable.Span,
                    typeRegistry.Register(symbol.Type),
                    SemanticSymbolProjector.GetSymbolId(symbol),
                    children));
            }

            SemanticOperation operation = declarations.Count == 1
                ? declarations[0]
                : CreateFlowOperation(
                    SemanticAsyncMethod.BlockOperationKind,
                    declaration.Span,
                    "type:void",
                    null,
                    declarations);
            projected = new SemanticAsyncStatement(operation, null);
            return true;
        }

        private bool TryProjectBool(
            ExpressionSyntax condition,
            out SemanticOperation? projected)
        {
            if (!TryProjectValue(condition, out projected))
            {
                return false;
            }
            if (projected!.TypeId == "type:bool")
            {
                return true;
            }
            diagnostics.Add(Error(
                "ASCS5415",
                "Controlled async CFG branch conditions must be supported bool expressions.",
                condition.Span));
            projected = null;
            failed = true;
            return false;
        }

        private bool TryProjectValueStatement(
            ExpressionSyntax expression,
            out SemanticAsyncStatement? projected)
        {
            if (!TryProjectValue(expression, out SemanticOperation? operation))
            {
                projected = null;
                return false;
            }
            projected = new SemanticAsyncStatement(operation!, null);
            return true;
        }

        private bool TryProjectValue(
            SyntaxNode syntax,
            out SemanticOperation? projected)
        {
            if (syntax.DescendantNodesAndSelf().OfType<AwaitExpressionSyntax>().Any())
            {
                diagnostics.Add(Error(
                    "ASCS5402",
                    "Controlled async CFG conditions and value expressions must remain synchronous.",
                    syntax.Span));
                projected = null;
                failed = true;
                return false;
            }
            IOperation? operation = semanticModel.GetOperation(syntax);
            if (operation is null || SemanticAsyncProjector.ContainsAsyncHelperOrTask(operation))
            {
                diagnostics.Add(Error(
                    "ASCS5403",
                    "Controlled async CFG requires a supported synchronous Roslyn operation.",
                    syntax.Span));
                projected = null;
                failed = true;
                return false;
            }

            int before = diagnostics.Count;
            projected = SemanticOperationProjector.ProjectAsyncStatementOperation(
                operation,
                context.PrimaryUnit,
                typeRegistry,
                diagnostics);
            if (diagnostics.Count == before
                && SemanticAsyncProjector.AllOperationsSupported(projected))
            {
                return true;
            }
            projected = null;
            failed = true;
            return false;
        }

        private int AddGoto(TextSpan span, int successor)
        {
            return AddDraft(
                span,
                Array.Empty<SemanticAsyncStatement>(),
                null,
                new DraftTransfer(
                    SemanticAsyncMethod.GotoTransferKind,
                    null,
                    successor,
                    -1));
        }

        private int AddDraft(
            TextSpan span,
            IReadOnlyList<SemanticAsyncStatement> statements,
            SemanticAsyncAwaitSite? awaitSite,
            DraftTransfer transfer)
        {
            if (drafts.Count >= SemanticAsyncMethod.MaximumControlFlowSegments)
            {
                diagnostics.Add(Error(
                    "ASCS5417",
                    $"Controlled async CFG exceeds the {SemanticAsyncMethod.MaximumControlFlowSegments}-segment method limit.",
                    span));
                failed = true;
                return -1;
            }
            int id = drafts.Count;
            drafts.Add(new DraftSegment(
                id,
                id,
                span,
                statements,
                awaitSite,
                transfer));
            return id;
        }

        private int Reject(string message, TextSpan span, string code = "ASCS5415")
        {
            diagnostics.Add(Error(code, message, span));
            failed = true;
            return -1;
        }

        private HashSet<int> CollectReachable(int entry)
        {
            HashSet<int> reachable = new();
            Stack<int> pending = new();
            pending.Push(entry);
            while (pending.Count > 0)
            {
                int current = pending.Pop();
                if (current < 0 || current >= drafts.Count || !reachable.Add(current))
                {
                    continue;
                }
                DraftTransfer transfer = drafts[current].Transfer;
                if (transfer.PrimaryTarget >= 0)
                {
                    pending.Push(transfer.PrimaryTarget);
                }
                if (transfer.SecondaryTarget >= 0)
                {
                    pending.Push(transfer.SecondaryTarget);
                }
            }
            return reachable;
        }

        private SemanticOperation CreateTrueLiteral(TextSpan span)
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

        private SemanticOperation CreateEquality(
            SemanticOperation left,
            SemanticOperation right,
            TextSpan span)
        {
            return new SemanticOperation(
                "binary",
                true,
                "equals",
                false,
                false,
                false,
                false,
                "type:bool",
                null,
                Array.Empty<string>(),
                null,
                null,
                null,
                null,
                null,
                SemanticSpanFactory.Create(context.PrimaryUnit.SourceText, span),
                new[] { left, right });
        }

        private SemanticOperation CreateFlowOperation(
            string kind,
            TextSpan span,
            string typeId,
            string? symbolId,
            IReadOnlyList<SemanticOperation> children)
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
                children);
        }

        private SemanticOperation CreateValueOperation(
            string kind,
            string typeId,
            string? symbolId,
            TextSpan span,
            IReadOnlyList<SemanticOperation>? children = null,
            string? operatorKind = null,
            SemanticConstant? constant = null)
        {
            return new SemanticOperation(
                kind,
                true,
                operatorKind,
                false,
                false,
                false,
                false,
                typeId,
                symbolId,
                Array.Empty<string>(),
                constant,
                null,
                null,
                null,
                null,
                SemanticSpanFactory.Create(context.PrimaryUnit.SourceText, span),
                children ?? Array.Empty<SemanticOperation>());
        }

        private SemanticDiagnostic Error(string code, string message, TextSpan span)
        {
            return new SemanticDiagnostic(
                code,
                "error",
                message,
                SemanticSpanFactory.Create(context.PrimaryUnit.SourceText, span));
        }

        private static int RemapTarget(
            int target,
            IReadOnlyDictionary<int, int> ordinalByDraft)
        {
            return target < 0 ? -1 : ordinalByDraft[target];
        }
    }

    private sealed class DraftSegment
    {
        public DraftSegment(
            int id,
            int sequence,
            TextSpan span,
            IReadOnlyList<SemanticAsyncStatement> statements,
            SemanticAsyncAwaitSite? awaitSite,
            DraftTransfer transfer)
        {
            Id = id;
            Sequence = sequence;
            Span = span;
            Statements = statements;
            AwaitSite = awaitSite;
            Transfer = transfer;
        }

        public int Id { get; }
        public int Sequence { get; }
        public TextSpan Span { get; }
        public IReadOnlyList<SemanticAsyncStatement> Statements { get; }
        public SemanticAsyncAwaitSite? AwaitSite { get; }
        public DraftTransfer Transfer { get; set; }
    }

    private sealed record DraftTransfer(
        string Kind,
        SemanticOperation? Condition,
        int PrimaryTarget,
        int SecondaryTarget);

    private sealed record SwitchCaseTarget(
        CaseSwitchLabelSyntax Label,
        SemanticOperation Value,
        int SectionTarget);

    private sealed record LoopTargets(int BreakTarget, int ContinueTarget)
    {
        public static LoopTargets None { get; } = new(-1, -1);
    }
}
