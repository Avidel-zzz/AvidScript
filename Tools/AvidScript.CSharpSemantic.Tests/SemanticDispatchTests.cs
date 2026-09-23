using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json.Nodes;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpSemantic;

internal static class SemanticDispatchTests
{
    public static int Run()
    {
        const string source = """
            using System;
            public interface IValue { int Read(); }
            public abstract class Base { public abstract int Read(); public virtual int Other() => 9; }
            public class Middle : Base { public override int Read() => 1; }
            public sealed class Derived : Middle, IValue {
                public sealed override int Read() => 2;
                int IValue.Read() => 3;
                public int Plain() => 4;
                public static int Static() => 5;
                public int Calls(Base value, IValue contract) {
                    Func<int> inherited = base.Read;
                    Func<int> current = Read;
                    Func<int> local = () => 7;
                    return value.Read() + contract.Read() + base.Read() + Plain() + Static() + inherited() + current() + local();
                }
            }
            public class Hidden : Middle { public new virtual int Read() => 4; }
            public class Leaf : Hidden { public override int Read() => 5; }
            """;
        const string path = "Scripts/MethodDispatch.cs";
        SemanticDocument document = Analyze(source);
        int count = 0;
        Check(document.SchemaVersion == 31 && document.SemanticVersion == "1.38", "dispatch has a versioned schema");
        Check(SemanticDispatchContractValidator.IsValid(document), "projected dispatch facts satisfy their contract: "
            + string.Join("\n", document.Callables.Select(item => item.MethodSymbolId + " static=" + item.IsStatic + " " + item.Dispatch))
            + "\nOperations:\n" + string.Join("\n", document.Methods.SelectMany(item => Walk(item.Root))
                .Where(item => item.Dispatch is not null).Select(item => item.SymbolId + " " + item.Dispatch)));
        SemanticCallable root = Find("Base.Read"), middle = Find("Middle.Read"), derived = Find("Derived.Read");
        Check(root.Dispatch is { IsAbstract: true, IsOverride: false } && !root.HasBody, "abstract declaration is not an executable body");
        Check(middle.Dispatch is { IsOverride: true, IsVirtual: false } && middle.Dispatch.OverriddenMethodSymbolId == root.MethodSymbolId
            && middle.Dispatch.SlotMethodSymbolId == root.MethodSymbolId, "override keeps immediate parent and original virtual slot");
        Check(derived.Dispatch is { IsOverride: true, IsSealed: true } && derived.Dispatch.OverriddenMethodSymbolId == middle.MethodSymbolId
            && derived.Dispatch.SlotMethodSymbolId == root.MethodSymbolId, "sealed override retains the original slot across multiple levels");
        Check(Find("Leaf.Read").Dispatch!.SlotMethodSymbolId == Find("Hidden.Read").MethodSymbolId,
            "new virtual starts an independent slot despite the same source name");
        Check(document.Callables.Single(item => item.Dispatch!.ExplicitInterfaceMethodIds.Count == 1)
            .Dispatch!.ExplicitInterfaceMethodIds.Single() == Find("IValue.Read").MethodSymbolId, "explicit interface method identity survives");
        SemanticOperation[] calls = Walk(document.Methods.Single(item => item.MethodSymbolId.Contains("Derived.Calls(", StringComparison.Ordinal)
                && item.MethodSymbolId.EndsWith(":int32", StringComparison.Ordinal)).Root)
            .Where(item => item.Dispatch is not null).ToArray();
        Check(calls.Any(item => item.Kind == "invocation" && item.Dispatch!.Kind == "virtual" && item.SymbolId == root.MethodSymbolId), "base-typed calls retain virtual dispatch");
        Check(calls.Any(item => item.Dispatch!.Kind == "interface" && item.SymbolId == Find("IValue.Read").MethodSymbolId), "interface calls retain a distinct route");
        Check(calls.Count(item => item.Dispatch!.IsBase && item.Dispatch.Kind == "direct" && item.SymbolId == middle.MethodSymbolId) == 2,
            "base invocation and base method group both bind directly to the base implementation");
        Check(calls.Any(item => item.Dispatch!.Kind == "static") && calls.Any(item => item.Dispatch!.Kind == "delegate")
            && calls.Any(item => item.Dispatch!.Kind == "direct" && item.SymbolId == Find("Derived.Plain").MethodSymbolId), "static, delegate and ordinary instance routes stay distinct");
        byte[] bytes = SemanticSerializer.Serialize(document);
        Check(bytes.SequenceEqual(SemanticSerializer.Serialize(Analyze(source)))
            && bytes.SequenceEqual(SemanticSerializer.Serialize(SemanticSerializer.Deserialize(bytes))), "dispatch facts serialize deterministically");
        JsonObject record = JsonNode.Parse(bytes)!["callables"]![0]!["dispatch"]!.AsObject();
        foreach (string name in record.Select(pair => pair.Key).ToArray())
        {
            JsonNode json = JsonNode.Parse(bytes)!;
            json["callables"]![0]!["dispatch"]!.AsObject().Remove(name);
            bool rejected = false;
            try { SemanticSerializer.Deserialize(Encoding.UTF8.GetBytes(json.ToJsonString())); }
            catch (InvalidDataException) { rejected = true; }
            Check(rejected, "omitted method dispatch fact must not default: " + name);
        }
        foreach (string name in new[] { "kind", "slot_method_symbol_id", "is_base" })
        {
            JsonNode json = JsonNode.Parse(bytes)!;
            FindOperationDispatch(json)!.Remove(name);
            bool rejected = false;
            try { SemanticSerializer.Deserialize(Encoding.UTF8.GetBytes(json.ToJsonString())); }
            catch (InvalidDataException) { rejected = true; }
            Check(rejected, "omitted operation dispatch fact must not default: " + name);
        }
        foreach (SemanticCallableDispatch? invalid in new SemanticCallableDispatch?[]
        {
            null, derived.Dispatch! with { OverriddenMethodSymbolId = null },
            derived.Dispatch! with { SlotMethodSymbolId = derived.MethodSymbolId },
            derived.Dispatch! with { IsAbstract = true }, derived.Dispatch! with { IsVirtual = true },
            derived.Dispatch! with { ExplicitInterfaceMethodIds = null! },
            derived.Dispatch! with { ExplicitInterfaceMethodIds = new[] { "x", "x" } },
        }) Check(!SemanticDispatchContractValidator.IsValid(document with { Callables = document.Callables.Select(item => item == derived
            ? item with { Dispatch = invalid } : item).ToArray() }), "malformed declaration dispatch must fail closed");
        Check(!SemanticDispatchContractValidator.IsValid(document with { SchemaVersion = 24, SemanticVersion = "1.28" }), "dispatch cannot be smuggled into older schemas");
        Check(!SemanticDispatchContractValidator.IsValid(document with { SchemaVersion = SemanticContract.CurrentSchemaVersion + 1 }), "future dispatch schema is not implicitly accepted");
        return count;

        SemanticCallable Find(string name) => document.Callables.Single(item => item.MethodSymbolId.Contains("global::" + name + "(", StringComparison.Ordinal));
        void Check(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); ++count; }
        SemanticDocument Analyze(string text)
        {
            FrontendDocument frontend = FrontendAnalyzer.Analyze(text, path);
            return SemanticAnalyzer.Analyze(text, path, frontend.Source.Sha256);
        }
    }

    private static IEnumerable<SemanticOperation> Walk(SemanticOperation operation)
    {
        yield return operation;
        foreach (SemanticOperation child in operation.Children)
            foreach (SemanticOperation descendant in Walk(child)) yield return descendant;
    }

    private static JsonObject? FindOperationDispatch(JsonNode? node)
    {
        if (node is JsonObject value)
        {
            if (value["dispatch"] is JsonObject dispatch && dispatch.ContainsKey("kind")) return dispatch;
            foreach (JsonNode? child in value.Select(pair => pair.Value))
                if (FindOperationDispatch(child) is { } found) return found;
        }
        else if (node is JsonArray array)
            foreach (JsonNode? child in array)
                if (FindOperationDispatch(child) is { } found) return found;
        return null;
    }
}
