using System;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpSemantic;

internal static class SemanticDelegateTypeTests
{
    public static int Run()
    {
        const string source = """
            using System;
            public delegate Recursive Recursive();
            public delegate int Modify(ref int value, out long result, in float scale);
            public delegate ref readonly int Reader();
            public static class Script
            {
                public static long Apply(Func<int, int> first, Func<long, long> second) => first(2) + second(3);
            }
            """;
        const string sourceId = "Scripts/DelegateTypes.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        SemanticDocument document = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);
        Require(document.Succeeded, string.Join(" | ", document.Diagnostics.Select(item => item.Message)));
        Require(document.SchemaVersion == 31 && document.SemanticVersion == "1.38"
            && SemanticDelegateContractValidator.IsValid(document), "delegate signatures require the versioned schema-21 contract");
        SemanticDelegateType[] functions = document.DelegateTypes.Where(type => type.TypeId.Contains("System.Func<")).ToArray();
        Require(functions.Length == 2 && functions.Select(type => type.InvokeMethodSymbolId).Distinct().Count() == 2,
            "closed generic Invoke identities must not collapse to the generic definition");
        foreach (SemanticDelegateType function in functions)
        {
            Require(document.ControlFlowGraphs.SelectMany(graph => graph.Blocks)
                .Where(block => block.BranchValue is not null)
                .SelectMany(block => Descendants(block.BranchValue!))
                .Any(operation => operation.Kind == "invocation" && operation.SymbolId == function.InvokeMethodSymbolId),
                "bound invocation identity must exactly match the closed signature descriptor");
            Require(document.ControlFlowGraphs.SelectMany(graph => graph.Blocks)
                .Where(block => block.BranchValue is not null).SelectMany(block => Descendants(block.BranchValue!))
                .Where(operation => operation.Kind == "invocation" && operation.SymbolId == function.InvokeMethodSymbolId)
                .SelectMany(operation => operation.Children.Where(child => child.Kind == "argument"))
                .All(argument => argument.SymbolId?.StartsWith("symbol:parameter:" + function.InvokeMethodSymbolId + ":0:", StringComparison.Ordinal) == true),
                "delegate arguments must retain closed Invoke parameter identities");
        }
        SemanticDelegateType recursive = document.DelegateTypes.Single(type => type.TypeId == "type:global::Recursive");
        Require(recursive.ReturnTypeId == recursive.TypeId, "recursive delegate signatures must terminate and retain nominal identity");
        SemanticDelegateType modify = document.DelegateTypes.Single(type => type.TypeId == "type:global::Modify");
        Require(modify.Parameters.Select(parameter => parameter.RefKind).SequenceEqual(new[] { "ref", "out", "in" }),
            "ref/out/in are part of the invocation signature");
        Require(document.DelegateTypes.Single(type => type.TypeId == "type:global::Reader").ReturnRefKind == "ref_readonly",
            "return reference semantics must not be lost");
        byte[] serialized = SemanticSerializer.Serialize(document);
        SemanticDocument roundTrip = SemanticSerializer.Deserialize(serialized);
        Require(SemanticDelegateContractValidator.IsValid(roundTrip)
            && serialized.SequenceEqual(SemanticSerializer.Serialize(roundTrip)),
            "signature serialization must be deterministic and complete");
        return 6;
    }

    private static System.Collections.Generic.IEnumerable<SemanticOperation> Descendants(SemanticOperation operation)
    {
        yield return operation;
        foreach (SemanticOperation child in operation.Children)
            foreach (SemanticOperation descendant in Descendants(child)) yield return descendant;
    }

    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}
