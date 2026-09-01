using System;
using System.Collections.Generic;
using System.Security.Cryptography;
using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Text;

namespace AvidScript.CSharpSemantic;

public sealed record SemanticCompilerWorkspaceSnapshot(
    int MetadataReferenceSetBuilds,
    int SyntaxTreeCacheHits,
    int SyntaxTreeCacheMisses,
    int SyntaxTreeCacheEntries);

public sealed class SemanticCompilerWorkspace
{
    public const int DefaultSyntaxTreeCapacity = 64;

    private sealed record CachedSyntaxTree(
        SyntaxTree SyntaxTree,
        SourceText SourceText,
        LinkedListNode<string> RecencyNode);

    private readonly object gate = new();
    private readonly int syntaxTreeCapacity;
    private readonly Dictionary<string, CachedSyntaxTree> syntaxTrees = new(StringComparer.Ordinal);
    private readonly LinkedList<string> syntaxTreeRecency = new();
    private IReadOnlyList<MetadataReference>? metadataReferences;
    private int metadataReferenceSetBuilds;
    private int syntaxTreeCacheHits;
    private int syntaxTreeCacheMisses;

    public SemanticCompilerWorkspace(int syntaxTreeCapacity = DefaultSyntaxTreeCapacity)
    {
        if (syntaxTreeCapacity < 1 || syntaxTreeCapacity > 1024)
        {
            throw new ArgumentOutOfRangeException(
                nameof(syntaxTreeCapacity),
                "Semantic syntax-tree capacity must be between 1 and 1024.");
        }
        this.syntaxTreeCapacity = syntaxTreeCapacity;
    }

    public SemanticCompilerWorkspaceSnapshot GetSnapshot()
    {
        lock (gate)
        {
            return new SemanticCompilerWorkspaceSnapshot(
                metadataReferenceSetBuilds,
                syntaxTreeCacheHits,
                syntaxTreeCacheMisses,
                syntaxTrees.Count);
        }
    }

    internal IReadOnlyList<MetadataReference> GetMetadataReferences()
    {
        lock (gate)
        {
            if (metadataReferences is null)
            {
                metadataReferences = SemanticReferenceResolver.ResolveTrustedPlatformAssemblies();
                metadataReferenceSetBuilds++;
            }
            return metadataReferences;
        }
    }

    internal SemanticCompilationUnit GetOrParseSyntaxTree(
        string source,
        string sourceId,
        CSharpParseOptions parseOptions,
        bool isPrimary)
    {
        ArgumentNullException.ThrowIfNull(source);
        ArgumentException.ThrowIfNullOrWhiteSpace(sourceId);
        ArgumentNullException.ThrowIfNull(parseOptions);

        string sourceSha256 = Convert.ToHexString(
            SHA256.HashData(Encoding.UTF8.GetBytes(source))).ToLowerInvariant();
        string key = sourceId + "\n" + sourceSha256;
        lock (gate)
        {
            if (syntaxTrees.TryGetValue(key, out CachedSyntaxTree? cached))
            {
                syntaxTreeRecency.Remove(cached.RecencyNode);
                syntaxTreeRecency.AddLast(cached.RecencyNode);
                syntaxTreeCacheHits++;
                return new SemanticCompilationUnit(cached.SyntaxTree, cached.SourceText, isPrimary);
            }

            SourceText sourceText = SourceText.From(source);
            SyntaxTree syntaxTree = CSharpSyntaxTree.ParseText(
                sourceText,
                parseOptions,
                sourceId);
            LinkedListNode<string> recencyNode = syntaxTreeRecency.AddLast(key);
            syntaxTrees.Add(key, new CachedSyntaxTree(syntaxTree, sourceText, recencyNode));
            syntaxTreeCacheMisses++;
            while (syntaxTrees.Count > syntaxTreeCapacity)
            {
                LinkedListNode<string> oldest = syntaxTreeRecency.First!;
                syntaxTreeRecency.RemoveFirst();
                syntaxTrees.Remove(oldest.Value);
            }
            return new SemanticCompilationUnit(syntaxTree, sourceText, isPrimary);
        }
    }
}
