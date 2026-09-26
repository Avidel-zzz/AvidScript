using System;
using System.Collections.Generic;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.FlowAnalysis;
using Microsoft.CodeAnalysis.Operations;

namespace AvidScript.CSharpSemantic;

internal static class SemanticStaticInitializerProjector
{
    public static SemanticDocument Project(SemanticCompilationContext context,
        SemanticTypeRegistry registry, SemanticDocument document)
    {
        if (context.Compilation.GetDiagnostics().Any(item => item.Severity == DiagnosticSeverity.Error)) return document;
        var types = new Dictionary<string, INamedTypeSymbol>(StringComparer.Ordinal);
        var fields = new Dictionary<string, List<SemanticStaticFieldInitialization>>(StringComparer.Ordinal);
        List<SemanticDiagnostic> diagnostics = new();
        foreach (SemanticCompilationUnit unit in context.ProjectionUnits)
        {
            SemanticModel model = context.Compilation.GetSemanticModel(unit.SyntaxTree);
            string source = unit.SourceText.ToString();
            string hash = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(source))).ToLowerInvariant();
            foreach (BaseTypeDeclarationSyntax declaration in unit.SyntaxTree.GetRoot().DescendantNodes().OfType<BaseTypeDeclarationSyntax>())
            {
                if (model.GetDeclaredSymbol(declaration) is not INamedTypeSymbol type
                    || type.TypeKind is not (TypeKind.Class or TypeKind.Struct)
                    || type.StaticConstructors.Length == 0) continue;
                string id = registry.Register(type);
                types.TryAdd(id, type);
                fields.TryAdd(id, new());
            }
            foreach (VariableDeclaratorSyntax variable in unit.SyntaxTree.GetRoot().DescendantNodes().OfType<VariableDeclaratorSyntax>())
            {
                if (variable.Parent?.Parent is not FieldDeclarationSyntax
                    || model.GetDeclaredSymbol(variable) is not IFieldSymbol { IsStatic: true, IsConst: false } field)
                    continue;
                string owner = registry.Register(field.ContainingType);
                if (!fields.TryGetValue(owner, out var declaredFields)) continue;
                SemanticOperation? initializer = null;
                SemanticControlFlowGraph? graph = null;
                if (variable.Initializer is not null)
                {
                    if (model.GetOperation(variable.Initializer) is not IFieldInitializerOperation operation)
                    {
                        Error("Static field initializer has no Roslyn operation.", unit, variable);
                        continue;
                    }
                    initializer = SemanticOperationProjector.ProjectAsyncStatementOperation(operation, unit, registry, diagnostics);
                    graph = SemanticControlFlowProjector.ProjectGraph(ControlFlowGraph.Create(operation),
                        field.ContainingType.StaticConstructors.Single(), unit, registry) with
                    { MethodSymbolId = SemanticStaticInitialization.InitializerId(SemanticSymbolProjector.GetSymbolId(field)) };
                }
                declaredFields.Add(new(SemanticSymbolProjector.GetSymbolId(field), unit.SyntaxTree.FilePath,
                    hash, source.Length, SemanticSpanFactory.Create(unit.SourceText, variable.Span), initializer, graph));
            }
        }
        if (types.Count == 0) return document;
        List<SemanticStaticTypeInitialization> plans = new();
        foreach (var entry in types.OrderBy(item => item.Key, StringComparer.Ordinal))
        {
            INamedTypeSymbol type = entry.Value;
            var initializers = fields[entry.Key];
            var expected = type.GetMembers().OfType<IFieldSymbol>().Where(field => field.IsStatic && !field.IsConst).ToArray();
            if (expected.Length != initializers.Count)
                diagnostics.Add(new("ASCS1071", "error",
                    "Every static field requires an executable source declaration; implicit storage and metadata-only partial fields cannot be omitted.",
                    SemanticSpanFactory.Empty));
            IMethodSymbol constructor = type.StaticConstructors.Single();
            plans.Add(new(entry.Key, constructor.IsImplicitlyDeclared,
                constructor.IsImplicitlyDeclared ? null : SemanticSymbolProjector.GetSymbolId(constructor), initializers));
        }
        return document with
        {
            SchemaVersion = SemanticStaticInitialization.SchemaVersion,
            SemanticVersion = SemanticStaticInitialization.SemanticVersion,
            StaticInitialization = new(document.SchemaVersion, document.SemanticVersion, plans),
            Types = registry.Build(), TypeShapes = registry.BuildShapes(), ClassTypes = registry.BuildClassTypes(),
            Succeeded = document.Succeeded && diagnostics.All(item => item.Severity != "error"),
            Diagnostics = document.Diagnostics.Concat(diagnostics).OrderBy(item => item.Span.Start)
                .ThenBy(item => item.Code, StringComparer.Ordinal).ToArray(),
        };

        void Error(string message, SemanticCompilationUnit unit, SyntaxNode syntax) =>
            diagnostics.Add(new("ASCS1071", "error", message, SemanticSpanFactory.Create(unit.SourceText, syntax.Span)));
    }
}
