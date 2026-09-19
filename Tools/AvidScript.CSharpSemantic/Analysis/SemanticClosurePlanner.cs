using System;
using System.Collections.Generic;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace AvidScript.CSharpSemantic;

internal sealed record SemanticLexicalCapture(string Id, string Owner, string Name, string TypeId,
    string Kind, SemanticSpan Span, bool IsReceiver = false);

internal sealed record SemanticClosureProjection(
    IReadOnlyList<SemanticClosureEnvironment> Environments,
    IReadOnlyList<SemanticClosureBinding> Bindings,
    IReadOnlyList<SemanticDiagnostic> Diagnostics);

internal static class SemanticClosurePlanner
{
    public static SemanticClosureProjection Project(SemanticCompilationContext context,
        IReadOnlyDictionary<string, SemanticLexicalCapture> captures,
        IReadOnlyDictionary<string, SortedSet<string>> required,
        IReadOnlyList<SemanticMethodBody> methods)
    {
        HashSet<string> targets = methods.SelectMany(method => Enumerate(method.Root))
            .Where(operation => operation.Kind == "method_reference" && operation.SymbolId is not null
                && required.ContainsKey(operation.SymbolId))
            .Select(operation => operation.SymbolId!).ToHashSet(StringComparer.Ordinal);
        HashSet<string> heapCells = targets.SelectMany(target => required[target]).ToHashSet(StringComparer.Ordinal);
        if (heapCells.Count == 0) return new(Array.Empty<SemanticClosureEnvironment>(),
            Array.Empty<SemanticClosureBinding>(), Array.Empty<SemanticDiagnostic>());

        Dictionary<string, SemanticExecutableBody> owners = SemanticExecutableBodyResolver.Resolve(context)
            .ToDictionary(body => SemanticSymbolProjector.GetSymbolId(body.Method), StringComparer.Ordinal);
        Dictionary<string, SyntaxNode> declarations = new(StringComparer.Ordinal);
        foreach (SemanticCompilationUnit unit in context.ProjectionUnits)
        {
            SemanticModel model = context.Compilation.GetSemanticModel(unit.SyntaxTree);
            foreach (SyntaxNode node in unit.SyntaxTree.GetRoot().DescendantNodes().Where(node =>
                node is VariableDeclaratorSyntax or ForEachStatementSyntax or SingleVariableDesignationSyntax))
                if (model.GetDeclaredSymbol(node) is ILocalSymbol local)
                    declarations[SemanticSymbolProjector.GetSymbolId(local)] = node;
        }

        List<SemanticDiagnostic> diagnostics = new();
        Dictionary<string, SemanticClosureEnvironment> environments = new(StringComparer.Ordinal);
        foreach (string id in heapCells.OrderBy(id => id, StringComparer.Ordinal))
        {
            SemanticLexicalCapture capture = captures[id];
            if (!owners.TryGetValue(capture.Owner, out SemanticExecutableBody? owner))
            {
                Reject(capture);
                continue;
            }
            SyntaxNode scope = owner.Declaration;
            string kind = "activation";
            int ordinal = 0;
            if (capture.Kind == "local_reference")
            {
                if (!declarations.TryGetValue(id, out SyntaxNode? declaration)) { Reject(capture); continue; }
                if (declaration is ForEachStatementSyntax each)
                {
                    scope = each;
                    kind = "foreach_iteration";
                }
                else if (declaration is VariableDeclaratorSyntax { Parent.Parent: ForStatementSyntax loop })
                {
                    scope = loop;
                    kind = "for_entry";
                }
                else if (declaration is VariableDeclaratorSyntax { Parent.Parent: LocalDeclarationStatementSyntax { Parent: BlockSyntax block } })
                {
                    // A method's top-level block and its parameters share an activation.
                    if (block.Parent != owner.Declaration) { scope = block; kind = "block_entry"; }
                }
                else { Reject(capture); continue; }
                if (kind != "activation")
                    ordinal = owner.Declaration.DescendantNodes().Where(node => node.RawKind == scope.RawKind
                        && node.Ancestors().FirstOrDefault(SemanticExecutableBodyResolver.IsExecutableDeclaration) == owner.Declaration)
                        .TakeWhile(node => node != scope).Count();
            }
            string environmentId = SemanticClosureEnvironment.GetId(capture.Owner, kind, ordinal);
            if (!environments.TryGetValue(environmentId, out SemanticClosureEnvironment? environment))
                environment = new(environmentId, capture.Owner, kind, ordinal,
                    SemanticSpanFactory.Create(owner.Unit.SourceText, scope.Span), Array.Empty<SemanticClosureCell>());
            environments[environmentId] = environment with { Cells = environment.Cells.Append(
                new SemanticClosureCell(id, capture.TypeId, capture.IsReceiver ? "receiver"
                    : capture.Kind == "parameter_reference" ? "parameter" : "local")).ToArray() };
        }
        // A partial plan cannot be consumed as an allocation contract.
        if (diagnostics.Count != 0) return new(Array.Empty<SemanticClosureEnvironment>(),
            Array.Empty<SemanticClosureBinding>(), diagnostics);
        SemanticClosureBinding[] bindings = required.OrderBy(pair => pair.Key, StringComparer.Ordinal)
            .Select(pair => new SemanticClosureBinding(pair.Key, targets.Contains(pair.Key),
                pair.Value.Where(heapCells.Contains).ToArray()))
            .Where(binding => binding.CellSymbolIds.Count != 0).ToArray();
        IReadOnlyList<SemanticClosureEnvironment> allocated = SemanticClosureAllocationProjector.Project(
            context, owners, environments.Values.OrderBy(environment => environment.Id, StringComparer.Ordinal).ToArray(), diagnostics);
        return diagnostics.Count == 0 ? new(allocated, bindings, diagnostics)
            : new(Array.Empty<SemanticClosureEnvironment>(), Array.Empty<SemanticClosureBinding>(), diagnostics);

        void Reject(SemanticLexicalCapture capture) => diagnostics.Add(new("ASCS4004", "error",
            $"Escaping capture '{capture.Name}' has an allocation scope that is not yet supported.", capture.Span));
    }

    private static IEnumerable<SemanticOperation> Enumerate(SemanticOperation operation)
    {
        yield return operation;
        foreach (SemanticOperation child in operation.Children)
            foreach (SemanticOperation descendant in Enumerate(child)) yield return descendant;
    }
}
