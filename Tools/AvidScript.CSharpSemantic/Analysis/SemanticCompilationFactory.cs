using System.Collections.Generic;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Text;

namespace AvidScript.CSharpSemantic;

internal sealed record SemanticCompilationUnit(
    SyntaxTree SyntaxTree,
    SourceText SourceText,
    bool IsPrimary);

internal sealed record SemanticCompilationContext(
    CSharpCompilation Compilation,
    SemanticCompilationUnit PrimaryUnit,
    IReadOnlyList<SemanticCompilationUnit> ProjectionUnits)
{
    public SyntaxTree SyntaxTree => PrimaryUnit.SyntaxTree;

    public SourceText SourceText => PrimaryUnit.SourceText;
}

internal static class SemanticCompilationFactory
{
    private static readonly CSharpParseOptions ParseOptions = new(
        languageVersion: LanguageVersion.CSharp12,
        documentationMode: DocumentationMode.Parse,
        kind: SourceCodeKind.Regular);

    private static readonly CSharpCompilationOptions CompilationOptions = new(
        OutputKind.DynamicallyLinkedLibrary,
        optimizationLevel: OptimizationLevel.Release,
        allowUnsafe: false,
        nullableContextOptions: NullableContextOptions.Enable,
        deterministic: true);

    public static SemanticCompilationContext Create(
        string source,
        string sourceId,
        IReadOnlyList<SemanticReferenceSource> referenceSources,
        SemanticCompilerWorkspace workspace)
    {
        SemanticCompilationUnit primaryUnit = workspace.GetOrParseSyntaxTree(
            source,
            sourceId,
            ParseOptions,
            isPrimary: true);
        List<SyntaxTree> syntaxTrees = new() { primaryUnit.SyntaxTree };
        List<SemanticCompilationUnit> projectionUnits = new() { primaryUnit };
        foreach (SemanticReferenceSource reference in referenceSources)
        {
            SemanticCompilationUnit referenceUnit = workspace.GetOrParseSyntaxTree(
                reference.Source,
                reference.SourceId,
                ParseOptions,
                isPrimary: false);
            syntaxTrees.Add(referenceUnit.SyntaxTree);
            if (reference.IsExecutable)
            {
                projectionUnits.Add(referenceUnit);
            }
        }

        CSharpCompilation compilation = CSharpCompilation.Create(
            "AvidScript.SemanticAnalysis",
            syntaxTrees,
            workspace.GetMetadataReferences(),
            CompilationOptions);
        return new SemanticCompilationContext(compilation, primaryUnit, projectionUnits);
    }
}
