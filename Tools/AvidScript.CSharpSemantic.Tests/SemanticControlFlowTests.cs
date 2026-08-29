using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpSemantic;

internal static class SemanticControlFlowTests
{
    private const string StructuredSource = """
        namespace Game;
        public sealed class Script
        {
            public int Run(int value)
            {
                int result = 0;
                if (value > 0)
                {
                    result += value;
                }
                else
                {
                    result -= value;
                }

                int index = 0;
                while (index < value)
                {
                    index++;
                    if (index == 2) { continue; }
                    if (index == 4) { break; }
                    result += index;
                }

                for (int loop = 0; loop < 2; loop++)
                {
                    result += loop;
                }

                return result;
            }
        }
        """;

    public static int Run()
    {
        StructuredControlFlowProducesStableBlocksAndEdges();
        ShortCircuitFlowCapturesRetainStableIds();
        PlainThrowControlFlowFailsClosed();
        ExceptionAsyncAndYieldControlFlowFailClosed();
        InvalidControlFlowDoesNotProducePartialGraphs();
        ControlFlowSerializationIsDeterministic();
        return 6;
    }

    private static void StructuredControlFlowProducesStableBlocksAndEdges()
    {
        SemanticDocument document = Analyze(StructuredSource, "Scripts/StructuredControlFlow.cs");

        Assert(document.Succeeded, "supported structured control flow should pass semantic analysis");
        Assert(document.SchemaVersion == 15 && document.SemanticVersion == "1.17",
            "current callable artifacts should advertise semantic schema v15 / version 1.17");
        SemanticControlFlowGraph graph = document.ControlFlowGraphs.Single(item =>
            item.MethodSymbolId == "symbol:method:global::Game.Script.Run(int32):int32");
        Assert(graph.Blocks.Select(block => block.Ordinal).SequenceEqual(Enumerable.Range(0, graph.Blocks.Count)),
            "CFG block ordinals should be contiguous and source independent");
        Assert(graph.Blocks.Count(block => block.Kind == "entry") == 1,
            "CFG should contain exactly one entry block");
        Assert(graph.Blocks.Count(block => block.Kind == "exit") == 1,
            "CFG should contain exactly one exit block");
        Assert(graph.Blocks.Single(block => block.Kind == "entry").Ordinal == graph.EntryBlockOrdinal,
            "entry_block_ordinal should reference the entry block");
        Assert(graph.Blocks.Single(block => block.Kind == "exit").Ordinal == graph.ExitBlockOrdinal,
            "exit_block_ordinal should reference the exit block");

        HashSet<int> ordinals = graph.Blocks.Select(block => block.Ordinal).ToHashSet();
        IReadOnlyList<SemanticControlFlowEdge> successors = graph.Blocks
            .SelectMany(block => block.Successors)
            .ToArray();
        IReadOnlyList<SemanticControlFlowEdge> predecessors = graph.Blocks
            .SelectMany(block => block.Predecessors)
            .ToArray();
        Assert(successors.All(edge => ordinals.Contains(edge.SourceBlockOrdinal) &&
            ordinals.Contains(edge.DestinationBlockOrdinal)),
            "every successor edge should reference valid blocks");
        Assert(predecessors.All(edge => ordinals.Contains(edge.SourceBlockOrdinal) &&
            ordinals.Contains(edge.DestinationBlockOrdinal)),
            "every predecessor edge should reference valid blocks");
        Assert(successors.Select(GetEdgeIdentity).OrderBy(identity => identity, StringComparer.Ordinal)
            .SequenceEqual(predecessors.Select(GetEdgeIdentity).OrderBy(identity => identity, StringComparer.Ordinal)),
            "predecessor and successor edge views should describe the same graph");
        Assert(successors.Any(edge => edge.DestinationBlockOrdinal <= edge.SourceBlockOrdinal),
            "loop CFG should contain at least one back edge");
        Assert(successors.Any(edge => edge.Semantics == "return" &&
            edge.DestinationBlockOrdinal == graph.ExitBlockOrdinal),
            "return should produce an explicit edge to the exit block");
        Assert(graph.Blocks.Any(block => block.ConditionKind is "when_true" or "when_false"),
            "conditional blocks should retain stable condition kinds");
        Assert(graph.Blocks.SelectMany(block => block.Operations).All(operation => operation.IsSupported),
            "supported CFG block operations should not contain unsupported lowering nodes");
    }

    private static void ShortCircuitFlowCapturesRetainStableIds()
    {
        const string source = """
            class Script
            {
                static bool Combine(bool left, bool right) => left && right;
                bool IsValid(int a, int b, int c, int d) =>
                    Combine(a > 0 && b > 0, c > 0 && d > 0);
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/ShortCircuitCapture.cs");

        Assert(document.Succeeded, "short-circuit boolean flow should be supported");
        SemanticControlFlowGraph graph = document.ControlFlowGraphs.Single(item =>
            item.MethodSymbolId == "symbol:method:global::Script.IsValid(int32,int32,int32,int32):bool");
        IReadOnlyList<SemanticOperation> operations = graph.Blocks
            .SelectMany(block => block.BranchValue is null
                ? block.Operations
                : block.Operations.Append(block.BranchValue))
            .SelectMany(Flatten)
            .ToArray();
        HashSet<string> captures = operations
            .Where(operation => operation.Kind == "flow_capture")
            .Select(operation => operation.CaptureId!)
            .ToHashSet(StringComparer.Ordinal);
        IReadOnlyList<string> references = operations
            .Where(operation => operation.Kind == "flow_capture_reference")
            .Select(operation => operation.CaptureId!)
            .ToArray();

        Assert(captures.Count >= 2 && captures.All(capture =>
            capture.StartsWith("capture:", StringComparison.Ordinal) &&
            capture["capture:".Length..].All(char.IsAsciiDigit)),
            "flow captures should retain AvidScript-owned numeric ids");
        Assert(references.Count > 0 && references.All(captures.Contains),
            "flow capture references should target a declared capture id");
    }

    private static void PlainThrowControlFlowFailsClosed()
    {
        const string blockSource = "class Script { void Fail() { throw new System.Exception(); } }";
        SemanticDocument blockDocument = Analyze(blockSource, "Scripts/BlockThrow.cs");
        Assert(blockDocument.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS3001"),
            "a destinationless block throw should emit ASCS3001");
        Assert(blockDocument.ControlFlowGraphs.Count == 0,
            "a destinationless block throw should clear all CFGs");

        const string expressionSource =
            "class Script { int Fail() => throw new System.Exception(); }";
        SemanticDocument expressionDocument = Analyze(expressionSource, "Scripts/ExpressionThrow.cs");
        Assert(expressionDocument.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS3001"),
            "an expression-bodied throw should emit ASCS3001");
        Assert(expressionDocument.ControlFlowGraphs.Count == 0,
            "an expression-bodied throw should clear all CFGs");
    }

    private static void ExceptionAsyncAndYieldControlFlowFailClosed()
    {
        const string source = """
            using System.Collections.Generic;
            using System.Threading.Tasks;

            public sealed class Script
            {
                public void ExceptionFlow()
                {
                    try { }
                    catch { }
                }

                public async Task AsyncFlow()
                {
                    await Task.Yield();
                }

                public IEnumerable<int> IteratorFlow()
                {
                    yield return 1;
                }
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/UnsupportedControlFlow.cs");

        Assert(!document.Succeeded, "exception, async, and yield control flow should fail closed");
        Assert(document.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS3001"),
            "exception regions should emit ASCS3001");
        Assert(document.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS3002"),
            "async methods should emit ASCS3002");
        Assert(document.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS3003"),
            "yield methods should emit ASCS3003");
        Assert(document.ControlFlowGraphs.Count == 0,
            "unsupported control-flow methods should not expose partial graphs");
    }

    private static void InvalidControlFlowDoesNotProducePartialGraphs()
    {
        const string source = "class Script { void Invalid() { break; } }";
        SemanticDocument document = Analyze(source, "Scripts/InvalidControlFlow.cs");

        Assert(!document.Succeeded, "invalid control flow should fail semantic analysis");
        Assert(document.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS3004"),
            "invalid CFG input should emit ASCS3004");
        Assert(document.ControlFlowGraphs.Count == 0,
            "invalid control flow should not expose a partial graph");
    }

    private static void ControlFlowSerializationIsDeterministic()
    {
        SemanticDocument first = Analyze(StructuredSource, "Scripts/DeterministicControlFlow.cs");
        SemanticDocument second = Analyze(StructuredSource, "Scripts/DeterministicControlFlow.cs");

        Assert(SemanticSerializer.Serialize(first).SequenceEqual(SemanticSerializer.Serialize(second)),
            "CFG semantic JSON should be byte-for-byte deterministic");
    }

    private static SemanticDocument Analyze(string source, string sourceId)
    {
        string hash = FrontendAnalyzer.Analyze(source, sourceId).Source.Sha256;
        return SemanticAnalyzer.Analyze(source, sourceId, hash);
    }

    private static IEnumerable<SemanticOperation> Flatten(SemanticOperation operation)
    {
        yield return operation;
        foreach (SemanticOperation child in operation.Children)
        {
            foreach (SemanticOperation descendant in Flatten(child))
            {
                yield return descendant;
            }
        }
    }

    private static string GetEdgeIdentity(SemanticControlFlowEdge edge)
    {
        return $"{edge.SourceBlockOrdinal}:{edge.DestinationBlockOrdinal}:{edge.Kind}:{edge.Semantics}";
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
