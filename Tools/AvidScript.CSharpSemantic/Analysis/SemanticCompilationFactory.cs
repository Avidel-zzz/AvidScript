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
    public static SemanticCompilationContext Create(
        string source,
        string sourceId,
        IReadOnlyList<SemanticReferenceSource> referenceSources)
    {
        SourceText sourceText = SourceText.From(source);
        CSharpParseOptions parseOptions = new(
            languageVersion: LanguageVersion.CSharp12,
            documentationMode: DocumentationMode.Parse,
            kind: SourceCodeKind.Regular);
        SyntaxTree syntaxTree = CSharpSyntaxTree.ParseText(sourceText, parseOptions, sourceId);
        SemanticCompilationUnit primaryUnit = new(syntaxTree, sourceText, true);
        List<SyntaxTree> syntaxTrees = new() { syntaxTree };
        List<SemanticCompilationUnit> projectionUnits = new() { primaryUnit };
        foreach (SemanticReferenceSource reference in referenceSources)
        {
            SourceText referenceText = SourceText.From(reference.Source);
            SyntaxTree referenceTree = CSharpSyntaxTree.ParseText(
                referenceText,
                parseOptions,
                reference.SourceId);
            syntaxTrees.Add(referenceTree);
            if (reference.IsExecutable)
            {
                projectionUnits.Add(new SemanticCompilationUnit(
                    referenceTree,
                    referenceText,
                    false));
            }
        }

        CSharpCompilationOptions compilationOptions = new(
            OutputKind.DynamicallyLinkedLibrary,
            optimizationLevel: OptimizationLevel.Release,
            allowUnsafe: false,
            nullableContextOptions: NullableContextOptions.Enable,
            deterministic: true);
        CSharpCompilation compilation = CSharpCompilation.Create(
            "AvidScript.SemanticAnalysis",
            syntaxTrees,
            SemanticReferenceResolver.ResolveTrustedPlatformAssemblies(),
            compilationOptions);
        return new SemanticCompilationContext(compilation, primaryUnit, projectionUnits);
    }
}
