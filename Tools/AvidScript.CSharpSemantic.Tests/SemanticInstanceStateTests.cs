using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpSemantic;

internal static class SemanticInstanceStateTests
{
    public static int Run()
    {
        InstanceStateSharesContainingTypeIdentity();
        LambdaAndClosureFailClosed();
        DynamicDispatchFailsClosed();
        UnsafeCodeFailsClosed();
        DocumentLevelPolicyErrorsClearAllGraphs();
        return 5;
    }

    private static void InstanceStateSharesContainingTypeIdentity()
    {
        const string source = """
            namespace Game;
            public sealed class Counter
            {
                private int Value;

                public Counter(int initial)
                {
                    this.Value = initial;
                }

                public int Increment(int amount)
                {
                    Value += amount;
                    return this.Read();
                }

                private int Read() => this.Value;
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/InstanceCounter.cs");

        const string typeId = "type:global::Game.Counter";
        const string typeSymbolId = "symbol:type:global::Game.Counter";
        const string fieldId = "symbol:field:global::Game.Counter.Value:int32";
        const string constructorId = "symbol:method:global::Game.Counter..ctor(int32):void";
        const string incrementId = "symbol:method:global::Game.Counter.Increment(int32):int32";
        const string readId = "symbol:method:global::Game.Counter.Read():int32";

        Assert(document.Succeeded, "supported instance state should pass semantic analysis");
        Assert(document.SchemaVersion == 6 && document.SemanticVersion == "1.6",
            "state contract artifacts should use schema v6 / semantic version 1.6");
        SemanticSymbol field = document.Symbols.Single(symbol => symbol.Id == fieldId);
        Assert(!field.IsStatic && field.ContainingSymbolId == typeSymbolId,
            "instance fields should retain their containing type identity");
        SemanticSymbol constructor = document.Symbols.Single(symbol => symbol.Id == constructorId);
        Assert(constructor.Kind == "constructor" && !constructor.IsStatic &&
            constructor.ContainingSymbolId == typeSymbolId,
            "constructors should retain their instance containing type identity");
        IReadOnlyList<SemanticSymbol> instanceMethods = document.Symbols
            .Where(symbol => symbol.Id is incrementId or readId)
            .ToArray();
        Assert(instanceMethods.Count == 2 && instanceMethods.All(symbol =>
            !symbol.IsStatic && symbol.ContainingSymbolId == typeSymbolId),
            "ordinary instance methods should share the same containing type identity");

        IReadOnlyList<SemanticOperation> operations = document.Methods
            .Where(method => method.MethodSymbolId is constructorId or incrementId or readId)
            .SelectMany(method => Flatten(method.Root))
            .ToArray();
        IReadOnlyList<SemanticOperation> instanceReferences = operations
            .Where(operation => operation.Kind == "instance_reference")
            .ToArray();
        Assert(instanceReferences.Count > 0 && instanceReferences.All(operation =>
            operation.TypeId == typeId && operation.SymbolId == typeSymbolId),
            "explicit and implicit instance references should bind the containing type symbol");
        Assert(operations.Any(operation => operation.Kind == "field_reference" &&
            operation.SymbolId == fieldId),
            "instance field references should bind the complete field symbol");
        Assert(operations.Any(operation => operation.Kind == "invocation" &&
            operation.SymbolId == readId),
            "instance helper calls should bind the Roslyn-selected method symbol");
        Assert(document.ControlFlowGraphs.Select(graph => graph.MethodSymbolId)
            .Intersect(new[] { constructorId, incrementId, readId }, StringComparer.Ordinal)
            .Count() == 3,
            "every supported instance executable body should expose a CFG");
    }

    private static void LambdaAndClosureFailClosed()
    {
        const string source = """
            using System;
            class Script
            {
                int Run(int value)
                {
                    Func<int, int> add = item => item + value;
                    return add(value);
                }
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/Closure.cs");

        AssertFailsClosed(document, "ASCS4001", "lambda/closure");

        const string localFunctionSource =
            "class Script { int Run() { int Local() => 1; return Local(); } }";
        SemanticDocument localFunctionDocument = Analyze(
            localFunctionSource,
            "Scripts/LocalFunction.cs");
        AssertFailsClosed(localFunctionDocument, "ASCS4001", "a local function");
    }

    private static void DynamicDispatchFailsClosed()
    {
        const string source = "class Script { object Run(dynamic value) => value.Missing(); }";
        SemanticDocument document = Analyze(source, "Scripts/Dynamic.cs");

        AssertFailsClosed(document, "ASCS4002", "dynamic dispatch");
    }

    private static void UnsafeCodeFailsClosed()
    {
        const string source = "unsafe class Script { int Read(int* value) => *value; }";
        SemanticDocument document = Analyze(source, "Scripts/Unsafe.cs");

        AssertFailsClosed(document, "ASCS4003", "unsafe code");
    }

    private static void DocumentLevelPolicyErrorsClearAllGraphs()
    {
        const string source = "class Script { dynamic Value; int Safe() => 1; }";
        SemanticDocument document = Analyze(source, "Scripts/DynamicField.cs");

        AssertFailsClosed(document, "ASCS4002", "a document-level dynamic type");
    }

    private static void AssertFailsClosed(
        SemanticDocument document,
        string diagnosticCode,
        string feature)
    {
        Assert(!document.Succeeded, $"{feature} should fail semantic analysis");
        Assert(document.Diagnostics.Any(diagnostic =>
            diagnostic.Code == diagnosticCode && diagnostic.Span.Length > 0),
            $"{feature} should emit {diagnosticCode} with a source span");
        Assert(document.ControlFlowGraphs.Count == 0,
            $"{feature} should not expose CFGs to Phase 41");
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

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
