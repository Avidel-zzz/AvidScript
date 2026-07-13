using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Text;

namespace AvidScript.CSharpSemantic;

internal sealed record SemanticCompilationContext(
    CSharpCompilation Compilation,
    SyntaxTree SyntaxTree,
    SourceText SourceText);

internal static class SemanticCompilationFactory
{
    public static SemanticCompilationContext Create(string source, string sourceId)
    {
        SourceText sourceText = SourceText.From(source);
        CSharpParseOptions parseOptions = new(
            languageVersion: LanguageVersion.CSharp12,
            documentationMode: DocumentationMode.Parse,
            kind: SourceCodeKind.Regular);
        SyntaxTree syntaxTree = CSharpSyntaxTree.ParseText(sourceText, parseOptions, sourceId);
        CSharpCompilationOptions compilationOptions = new(
            OutputKind.DynamicallyLinkedLibrary,
            optimizationLevel: OptimizationLevel.Release,
            allowUnsafe: false,
            nullableContextOptions: NullableContextOptions.Enable,
            deterministic: true);
        CSharpCompilation compilation = CSharpCompilation.Create(
            "AvidScript.SemanticAnalysis",
            new[] { syntaxTree },
            SemanticReferenceResolver.ResolveTrustedPlatformAssemblies(),
            compilationOptions);
        return new SemanticCompilationContext(compilation, syntaxTree, sourceText);
    }
}
