using System;
using System.Collections.Generic;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.FlowAnalysis;
using Microsoft.CodeAnalysis.Operations;

namespace AvidScript.CSharpSemantic;

internal sealed record SemanticInstanceInitializerProjection(
    IReadOnlyList<SemanticCallable> Callables,
    IReadOnlyList<SemanticMethodBody> Methods,
    IReadOnlyList<SemanticControlFlowGraph> Graphs,
    IReadOnlyList<SemanticDiagnostic> Diagnostics);

// Field initialization uses ordinary calls, stores and CFGs. Keeping each class's
// initialization in its own function isolates Roslyn flow captures from the ctor
// and makes helper calls visible to reachability and generic specialization.
internal static class SemanticInstanceInitializerNormalizer
{
    public static SemanticInstanceInitializerProjection Normalize(
        SemanticCompilationContext context, SemanticTypeRegistry types,
        IReadOnlyList<SemanticCallable> sourceCallables,
        IReadOnlyList<SemanticMethodBody> sourceMethods,
        IReadOnlyList<SemanticControlFlowGraph> sourceGraphs)
    {
        var callables = sourceCallables.ToDictionary(item => item.MethodSymbolId, StringComparer.Ordinal);
        var methods = sourceMethods.ToDictionary(item => item.MethodSymbolId, StringComparer.Ordinal);
        var graphs = sourceGraphs.ToDictionary(item => item.MethodSymbolId, StringComparer.Ordinal);
        List<SemanticDiagnostic> diagnostics = new();
        if (context.Compilation.GetDiagnostics().Any(item => item.Severity == DiagnosticSeverity.Error))
            return new(sourceCallables, sourceMethods, sourceGraphs, diagnostics);

        var byType = new Dictionary<string, List<(IFieldInitializerOperation Operation, SemanticCompilationUnit Unit)>>(StringComparer.Ordinal);
        // Build this index once; large source units must not be rescanned per class.
        foreach (SemanticCompilationUnit fieldUnit in context.ProjectionUnits)
        {
            SemanticModel fieldModel = context.Compilation.GetSemanticModel(fieldUnit.SyntaxTree);
            foreach (VariableDeclaratorSyntax variable in fieldUnit.SyntaxTree.GetRoot()
                .DescendantNodes().OfType<VariableDeclaratorSyntax>())
            {
                if (variable.Initializer is null || variable.Parent?.Parent is not FieldDeclarationSyntax
                    || fieldModel.GetDeclaredSymbol(variable) is not IFieldSymbol { IsStatic: false } field
                    || field.ContainingType.TypeKind != TypeKind.Class
                    || field.ContainingType.BaseType?.SpecialType != SpecialType.System_Object) continue;
                if (fieldModel.GetOperation(variable.Initializer) is not IFieldInitializerOperation initializer)
                {
                    diagnostics.Add(new("ASCS1070", "error", "Instance field initializer has no Roslyn operation.",
                        SemanticSpanFactory.Create(fieldUnit.SourceText, variable.Span)));
                    continue;
                }
                string owner = types.Register(field.ContainingType);
                if (!byType.TryGetValue(owner, out var fields)) byType.Add(owner, fields = new());
                fields.Add((initializer, fieldUnit));
            }
        }
        var visited = new HashSet<string>(StringComparer.Ordinal);
        foreach (SemanticCompilationUnit unit in context.ProjectionUnits)
        {
            SemanticModel model = context.Compilation.GetSemanticModel(unit.SyntaxTree);
            foreach (ClassDeclarationSyntax declaration in unit.SyntaxTree.GetRoot()
                .DescendantNodes().OfType<ClassDeclarationSyntax>())
            {
                if (model.GetDeclaredSymbol(declaration) is not INamedTypeSymbol type
                    || type.IsStatic || declaration.ParameterList is not null
                    || type.BaseType?.SpecialType != SpecialType.System_Object)
                    continue;
                string typeId = types.Register(type);
                if (!visited.Add(typeId) || !byType.TryGetValue(typeId, out var initializers)) continue;
                // A partial declaration in a metadata-only reference cannot have
                // its initializer silently omitted from the executable type.
                int declaredCount = type.GetMembers().OfType<IFieldSymbol>().Count(field => !field.IsStatic
                    && field.DeclaringSyntaxReferences.Any(reference => reference.GetSyntax()
                        is VariableDeclaratorSyntax { Initializer: not null }));
                if (declaredCount != initializers.Count)
                {
                    diagnostics.Add(new("ASCS1070", "error", "All instance field initializers must have executable source.",
                        SemanticSpanFactory.Create(unit.SourceText, declaration.Span)));
                    continue;
                }
                IMethodSymbol constructor = type.InstanceConstructors[0];
                string helperId = SemanticInstanceInitializerContract.MethodId(typeId);
                SemanticSpan span = SemanticSpanFactory.Create(unit.SourceText, declaration.Span);
                var statements = initializers.Select(item => SemanticOperationProjector.ProjectAsyncStatementOperation(
                    item.Operation, item.Unit, types, diagnostics)).ToArray();
                var pieces = initializers.Select(item => SemanticControlFlowProjector.ProjectGraph(
                    ControlFlowGraph.Create(item.Operation), constructor, item.Unit, types)).ToArray();
                var helper = new SemanticCallable(helperId, typeId, types.Register(constructor.ReturnType),
                    Array.Empty<SemanticCallableParameter>(), false, false, true, null, null, null,
                    Dispatch: new(false, false, false, false, null, null, Array.Empty<string>()),
                    GenericTypeParameterIds: type.TypeParameters.Select(types.Register).ToArray());
                callables.Add(helperId, helper);
                methods.Add(helperId, new(helperId, Node("block", null, null, span, statements)));
                graphs.Add(helperId, Combine(helperId, pieces));

                foreach (IMethodSymbol ctor in type.InstanceConstructors)
                {
                    string ctorId = SemanticSymbolProjector.GetSymbolId(ctor);
                    if (!callables.TryGetValue(ctorId, out SemanticCallable? callable)) continue;
                    var syntax = ctor.DeclaringSyntaxReferences.FirstOrDefault()?.GetSyntax() as ConstructorDeclarationSyntax;
                    // this(...) reaches the helper through the terminal constructor; its arguments run first.
                    if (syntax?.Initializer?.IsKind(SyntaxKind.ThisConstructorInitializer) == true) continue;
                    SemanticOperation invocation = Node("invocation", "type:void", helperId, span,
                        Node("instance_reference", typeId, null, span)) with
                    {
                        TypeArgumentIds = helper.GenericTypeParameterIds!,
                        Dispatch = new("direct", null, false),
                    };
                    SemanticOperation statement = Node("expression_statement", null, null, span, invocation);
                    if (ctor.IsImplicitlyDeclared)
                    {
                        callables[ctorId] = callable with { HasBody = true };
                        methods.Add(ctorId, new(ctorId, Node("constructor_body", null, null, span, statement)));
                        graphs.Add(ctorId, StraightLine(ctorId, new[] { statement }));
                    }
                    else if (methods.TryGetValue(ctorId, out SemanticMethodBody? body)
                        && graphs.TryGetValue(ctorId, out SemanticControlFlowGraph? graph))
                    {
                        methods[ctorId] = body with { Root = body.Root with
                            { Children = new[] { statement }.Concat(body.Root.Children).ToArray() } };
                        graphs[ctorId] = Combine(ctorId, new[] { StraightLine(ctorId, new[] { statement }), graph });
                    }
                    else diagnostics.Add(new("ASCS1070", "error",
                        "Instance field initialization requires an executable constructor CFG.", span));
                }
            }
        }
        return new(callables.Values.OrderBy(item => item.MethodSymbolId, StringComparer.Ordinal).ToArray(),
            methods.Values.OrderBy(item => item.MethodSymbolId, StringComparer.Ordinal).ToArray(),
            graphs.Values.OrderBy(item => item.MethodSymbolId, StringComparer.Ordinal).ToArray(), diagnostics);
    }

    private static SemanticOperation Node(string kind, string? typeId, string? symbolId,
        SemanticSpan span, params SemanticOperation[] children) => new(kind, true, null, false,
            false, false, false, typeId, symbolId, Array.Empty<string>(), null, null, null, null, null, span, children);

    private static SemanticControlFlowGraph StraightLine(string id, IReadOnlyList<SemanticOperation> statements)
    {
        var first = new SemanticControlFlowEdge(0, 1, "fallthrough", "regular");
        var last = new SemanticControlFlowEdge(1, 2, "fallthrough", "regular");
        return new(id, 0, 2, new[] {
            new SemanticBasicBlock(0, "entry", true, "none", Array.Empty<SemanticOperation>(), null,
                Array.Empty<SemanticControlFlowEdge>(), new[] { first }),
            new SemanticBasicBlock(1, "block", true, "none", statements, null, new[] { first }, new[] { last }),
            new SemanticBasicBlock(2, "exit", true, "none", Array.Empty<SemanticOperation>(), null,
                new[] { last }, Array.Empty<SemanticControlFlowEdge>()) });
    }

    private static SemanticControlFlowGraph Combine(string id, IReadOnlyList<SemanticControlFlowGraph> pieces)
    {
        List<SemanticBasicBlock> blocks = new();
        int offset = 0;
        int entryOrdinal = pieces[0].EntryBlockOrdinal;
        int previousExit = -1;
        for (int index = 0; index < pieces.Count; index++)
        {
            SemanticControlFlowGraph piece = pieces[index];
            int start = offset;
            SemanticControlFlowEdge Edge(SemanticControlFlowEdge edge) => edge with
                { SourceBlockOrdinal = start + edge.SourceBlockOrdinal, DestinationBlockOrdinal = start + edge.DestinationBlockOrdinal };
            SemanticOperation Operation(SemanticOperation operation) => operation with
            {
                CaptureId = operation.CaptureId is null ? null : $"initializer:{index}:" + operation.CaptureId,
                Children = operation.Children.Select(Operation).ToArray(),
            };
            blocks.AddRange(piece.Blocks.Select(block => block with {
                Ordinal = start + block.Ordinal,
                Kind = block.Kind is "entry" or "exit" ? "block" : block.Kind,
                Operations = block.Operations.Select(Operation).ToArray(),
                BranchValue = block.BranchValue is null ? null : Operation(block.BranchValue),
                Predecessors = block.Predecessors.Select(Edge).ToArray(),
                Successors = block.Successors.Select(Edge).ToArray(),
            }));
            if (index > 0)
            {
                var edge = new SemanticControlFlowEdge(previousExit, start + piece.EntryBlockOrdinal, "fallthrough", "regular");
                int previous = blocks.FindIndex(block => block.Ordinal == previousExit);
                int entry = blocks.FindIndex(block => block.Ordinal == edge.DestinationBlockOrdinal);
                blocks[previous] = blocks[previous] with { Successors = new[] { edge } };
                blocks[entry] = blocks[entry] with { Predecessors = new[] { edge } };
            }
            previousExit = start + piece.ExitBlockOrdinal;
            offset += piece.Blocks.Max(block => block.Ordinal) + 1;
        }
        int entryIndex = blocks.FindIndex(block => block.Ordinal == entryOrdinal);
        int exitIndex = blocks.FindIndex(block => block.Ordinal == previousExit);
        blocks[entryIndex] = blocks[entryIndex] with { Kind = "entry" };
        blocks[exitIndex] = blocks[exitIndex] with { Kind = "exit" };
        return new(id, entryOrdinal, previousExit, blocks);
    }
}
