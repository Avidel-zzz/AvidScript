using System;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpSemantic;

internal static class SemanticCompilerWorkspaceTests
{
    public static int Run()
    {
        SyntaxTreesAreReusedAndBounded();
        return 1;
    }

    private static void SyntaxTreesAreReusedAndBounded()
    {
        const string sourceId = "Scripts/Workspace.cs";
        const string firstSource = "public static class Script { public static void Tick(float value) { } }";
        const string secondSource = "public static class Script { public static void Tick(double value) { } }";
        SemanticCompilerWorkspace workspace = new(syntaxTreeCapacity: 1);

        Analyze(firstSource, sourceId, workspace);
        SemanticCompilerWorkspaceSnapshot first = workspace.GetSnapshot();
        Assert(first.MetadataReferenceSetBuilds == 1, "workspace should build metadata references once");
        Assert(first.SyntaxTreeCacheHits == 0 && first.SyntaxTreeCacheMisses == 1,
            "first source should produce one syntax-tree miss");

        Analyze(firstSource, sourceId, workspace);
        SemanticCompilerWorkspaceSnapshot warm = workspace.GetSnapshot();
        Assert(warm.MetadataReferenceSetBuilds == 1 && warm.SyntaxTreeCacheHits == 1,
            "warm source should reuse metadata references and syntax tree");

        Analyze(secondSource, sourceId, workspace);
        Analyze(firstSource, sourceId, workspace);
        SemanticCompilerWorkspaceSnapshot evicted = workspace.GetSnapshot();
        Assert(evicted.MetadataReferenceSetBuilds == 1,
            "bounded syntax eviction must not rebuild metadata references");
        Assert(evicted.SyntaxTreeCacheMisses == 3 && evicted.SyntaxTreeCacheEntries == 1,
            "capacity-one workspace should evict old source versions");
    }

    private static void Analyze(
        string source,
        string sourceId,
        SemanticCompilerWorkspace workspace)
    {
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        _ = SemanticAnalyzer.Analyze(
            source,
            sourceId,
            frontend.Source.Sha256,
            Array.Empty<SemanticReferenceSource>(),
            workspace);
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
