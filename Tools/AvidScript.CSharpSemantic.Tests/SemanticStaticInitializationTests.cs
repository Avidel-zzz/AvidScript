using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json.Nodes;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpSemantic;

internal static class SemanticStaticInitializationTests
{
    public static int Run()
    {
        int count = 0;
        void Check(bool condition, string message)
        {
            if (!condition) throw new InvalidOperationException("Static source: " + message);
            count++;
        }
        SemanticDocument Analyze(string source, SemanticReferenceSource[]? references = null) =>
            SemanticAnalyzer.Analyze(source, "Scripts/Static.cs", FrontendAnalyzer.Analyze(source, "Scripts/Static.cs").Source.Sha256,
                references ?? Array.Empty<SemanticReferenceSource>(), new SemanticCompilerWorkspace(), enableStaticInitialization: true);
        SemanticDocument Project(string source, SemanticReferenceSource[]? references = null)
        {
            var document = Analyze(source, references);
            Check(document.SchemaVersion == 49 && document.SemanticVersion == "1.58", "outer identity");
            Check(document.Diagnostics.All(item => item.Severity != "error" || item.Code == "ASCS3001"),
                string.Join(" | ", document.Diagnostics.Select(item => item.Code + ": " + item.Message)));
            Check(SemanticStaticInitializationValidator.IsValid(document), "source plan validation: " + source);
            byte[] bytes = SemanticSerializer.Serialize(document);
            Check(bytes.SequenceEqual(SemanticSerializer.Serialize(Analyze(source, references))), "deterministic projection");
            var restored = SemanticSerializer.Deserialize(bytes);
            Check(bytes.SequenceEqual(SemanticSerializer.Serialize(restored))
                && SemanticStaticInitializationValidator.IsValid(restored), "canonical round trip");
            return document;
        }
        const string source = """
            public sealed class Value { public int Data; public Value(int value) { Data = value; } }
            public static class Log { public static int Trace; public static int Mark(int n) { Trace = Trace * 10 + n; return n; } }
            public class Cache {
                public const int Constant = 7;
                public static readonly Value Z = new Value(Log.Mark(1));
                public static int A = Log.Mark(2), Empty;
                public static Value Alias = Z;
                static Cache() { A += Log.Mark(3); }
                public static int Run() { return Alias.Data + A; }
            }
            """;
        SemanticDocument baseline = Project(source);
        var plan = baseline.StaticInitialization!;
        var type = plan.Types.Single(item => item.TypeId == "type:global::Cache");
        Check(plan.Types.Single(item => item.TypeId == "type:global::Log").Fields.Single().Initializer is null,
            "default-only storage participates without a generated constructor");
        Check(!type.BeforeFieldInit && type.ConstructorMethodId is not null, "explicit constructor trigger");
        Check(type.Fields.Select(field => baseline.Symbols.Single(symbol => symbol.Id == field.FieldSymbolId).Name)
            .SequenceEqual(new[] { "Z", "A", "Empty", "Alias" }), "source declaration order and const exclusion");
        Check(type.Fields[2].Initializer is null && type.Fields[2].ControlFlowGraph is null, "default-valued field");
        Check(type.Fields[3].Initializer!.Children[0].Children[1].SymbolId == type.Fields[0].FieldSymbolId,
            "alias retains its field dependency");
        var implicitType = Project("public static class Cache { public static int Value = 5; }").StaticInitialization!.Types.Single();
        Check(implicitType.BeforeFieldInit && implicitType.ConstructorMethodId is null, "beforefieldinit retained");
        Project("public class Cache { static Cache() {} }");
        Project("public struct Cache { public static int Value = 5; static Cache() {} }");
        Project("public class Cache<T> { public static T Value = default(T); static Cache() {} }");
        Project("public class A { public static int Value = B.Value + 1; static A() {} } public class B { public static int Value = A.Value + 2; static B() {} }");
        var conditional = Project("public static class Cache { public static bool Flag; public static int Z = Flag ? 1 : 2; }");
        Check(conditional.StaticInitialization!.Types.Single().Fields[1].ControlFlowGraph!.Blocks.Any(block => block.BranchValue is not null),
            "initializer keeps conditional CFG");
        Project("public class Cache { public static int Value = 5; static Cache() { throw new System.InvalidOperationException(); } }");
        var failure = Project("public class Cache { public static int Value; static Cache() { throw new System.InvalidOperationException(); } public static int Read() { try { return Value; } catch (System.Exception) { return -1; } } }");
        Check(failure.ClassTypes.Single(item => item.TypeId == "type:global::System.TypeInitializationException").BaseTypeId
            == "type:global::System.SystemException", "implicit initialization error retains Roslyn base type for catch dispatch");
        const string partial = "public partial class Cache { public static int Z = 1; static Cache() {} }";
        var additional = new[] { new SemanticReferenceSource("public partial class Cache { public static int A = 2; }", "Scripts/Part.cs", true) };
        var partialDoc = Project(partial, additional);
        Check(partialDoc.StaticInitialization!.Types.Single().Fields.Select(field => field.SourceId)
            .SequenceEqual(new[] { "Scripts/Static.cs", "Scripts/Part.cs" }), "partial input order and source identity");
        Check(Analyze(partial, new[] { additional[0] with { IsExecutable = false } }).Diagnostics.Any(item => item.Code == "ASCS1071"),
            "metadata-only partial field cannot disappear");
        Check(Analyze("public class Cache { public static int Value { get; set; } = 1; }").Diagnostics.Any(item => item.Code == "ASCS1071"),
            "implicit static storage is not silently dropped");
        var constants = Analyze("public static class Cache { public const int Value = 4; }");
        Check(constants.StaticInitialization is null && constants.SchemaVersion != 49, "constant-only source needs no initializer contract");
        var legacy = SemanticAnalyzer.Analyze(source, "Scripts/Static.cs", baseline.Source.Sha256);
        Check(legacy.StaticInitialization is null && !JsonNode.Parse(SemanticSerializer.Serialize(legacy))!.AsObject().ContainsKey("static_initialization"),
            "default API retains old serialized shape");

        SemanticDocument With(SemanticStaticTypeInitialization value) => baseline with { StaticInitialization = plan with {
            Types = plan.Types.Select(item => item.TypeId == type.TypeId ? value : item).ToArray() } };
        SemanticDocument Field(SemanticStaticFieldInitialization value) => With(type with { Fields = new[] { value }.Concat(type.Fields.Skip(1)).ToArray() });
        foreach (SemanticDocument invalid in new[] {
            baseline with { SchemaVersion = plan.BaseSchemaVersion, SemanticVersion = plan.BaseSemanticVersion },
            baseline with { SchemaVersion = 48, SemanticVersion = "1.57" },
            baseline with { SemanticVersion = "1.56" }, baseline with { StaticInitialization = null },
            baseline with { StaticInitialization = plan with { BaseSchemaVersion = 48, BaseSemanticVersion = "1.57" } },
            baseline with { StaticInitialization = plan with { BaseSchemaVersion = 49, BaseSemanticVersion = "1.58" } },
            baseline with { StaticInitialization = plan with { BaseSemanticVersion = "wrong" } },
            baseline with { StaticInitialization = plan with { Types = Array.Empty<SemanticStaticTypeInitialization>() } },
            baseline with { StaticInitialization = plan with { Types = null! } },
            baseline with { StaticInitialization = plan with { Types = new SemanticStaticTypeInitialization[] { null! } } },
            baseline with { StaticInitialization = plan with { Types = new[] { type, type } } },
            With(type with { Fields = null! }), With(type with { Fields = new SemanticStaticFieldInitialization[] { null! } }),
            With(type with { BeforeFieldInit = true }), With(type with { ConstructorMethodId = "missing" }),
            With(type with { Fields = type.Fields.Skip(1).ToArray() }),
            With(type with { Fields = type.Fields.Reverse().ToArray() }),
            Field(type.Fields[0] with { SourceSha256 = new string('0', 64) }),
            Field(type.Fields[0] with { SourceLength = 0 }), Field(type.Fields[0] with { ControlFlowGraph = null }),
            Field(type.Fields[0] with { Initializer = type.Fields[0].Initializer! with { Children = new SemanticOperation[] { null! } } }),
            Field(type.Fields[0] with { Initializer = type.Fields[0].Initializer! with { Children = null! } }),
            Field(type.Fields[0] with { ControlFlowGraph = type.Fields[0].ControlFlowGraph! with { MethodSymbolId = "missing" } }),
            Field(type.Fields[0] with { ControlFlowGraph = type.Fields[0].ControlFlowGraph! with { Blocks = Array.Empty<SemanticBasicBlock>() } }),
            Field(type.Fields[0] with { ControlFlowGraph = type.Fields[0].ControlFlowGraph! with { Blocks = new SemanticBasicBlock[] { null! } } }),
            Field(type.Fields[0] with { ControlFlowGraph = type.Fields[0].ControlFlowGraph! with {
                Blocks = type.Fields[0].ControlFlowGraph!.Blocks.Select(block => block with { Operations = Array.Empty<SemanticOperation>() }).ToArray() } }),
        }) Check(!SemanticStaticInitializationValidator.IsValid(invalid), "malformed plan accepted");
        var defaults = Project("public class Cache<T> { public static T Value; } public static class Script { public static int Main() { Cache<int>.Value = 4; Cache<long>.Value = 8; return Cache<int>.Value + (int)Cache<long>.Value; } }");
        var accesses = defaults.Methods.SelectMany(method => Operations(method.Root))
            .Where(operation => operation.Kind == "field_reference").ToArray();
        Check(accesses.Select(operation => operation.StaticFieldOwnerTypeId).Distinct().Count() == 2
            && accesses.Select(operation => operation.SymbolId).Distinct().Count() == 1,
            "closed owners are distinct while the field declaration remains shared");
        Check(defaults.StaticInitialization!.Types.Single().BeforeFieldInit
            && defaults.StaticInitialization.Types.Single().Fields.Single().Initializer is null,
            "generic default storage needs no synthetic initializer");
        Check(defaults.Types.Any(item => item.Id == "type:global::Cache<int>")
            && defaults.Types.Any(item => item.Id == "type:global::Cache<long>"), "closed owner types are registered");
        var generic = Project("public class Cache<T> { public static int Value; } public static class Script { public static int Read<T>() { return Cache<T>.Value; } public static int Main() { return Read<int>() + Read<long>(); } }");
        var specialized = generic.Callables.Where(item => item.GenericDefinitionSymbolId is not null).ToArray();
        Check(specialized.Length == 2, "both generic callers specialize");
        Check(generic.Methods.Where(method => specialized.Any(callable => callable.MethodSymbolId == method.MethodSymbolId))
            .SelectMany(method => Operations(method.Root)).Where(operation => operation.Kind == "field_reference")
            .Select(operation => operation.StaticFieldOwnerTypeId).ToHashSet().SetEquals(new[] {
                "type:global::Cache<int>", "type:global::Cache<long>" }), "generic body closes static owner through Roslyn");
        Project("public class Cache<T> { public static T Value; public static T Read() { return Value; } } public static class Script { public static int Main() { return Cache<int>.Read() + (int)Cache<long>.Read(); } }");
        Project("public class Base<T> { public static T Value; } public class Derived : Base<int> {} public static class Script { public static int Main() { return Derived.Value; } }");
        Project("public class Cache<T> { public static T Value; } public static class Script { public static int Read<T>() { return Cache<T[]>.Value.Length; } public static int Main() { return Read<int>(); } }");
        foreach (var owner in new string?[] { null, "", "type:missing", "type:global::Script", "type:global::Cache<long>" })
        {
            SemanticOperation Change(SemanticOperation operation) => operation with {
                StaticFieldOwnerTypeId = operation.Kind == "field_reference" && operation.TypeId == "type:int32"
                    ? owner : operation.StaticFieldOwnerTypeId,
                Children = operation.Children.Select(Change).ToArray() };
            Check(!SemanticStaticInitializationValidator.IsValid(defaults with {
                Methods = defaults.Methods.Select(method => method with { Root = Change(method.Root) }).ToArray() }),
                "missing, unknown, unrelated or incorrectly substituted field owner accepted: " + owner);
            Check(!SemanticStaticInitializationValidator.IsValid(defaults with {
                ControlFlowGraphs = defaults.ControlFlowGraphs.Select(graph => graph with { Blocks = graph.Blocks.Select(block => block with {
                    Operations = block.Operations.Select(Change).ToArray(),
                    BranchValue = block.BranchValue is null ? null : Change(block.BranchValue) }).ToArray() }).ToArray() }),
                "CFG owner validation cannot be bypassed: " + owner);
        }
        Check(!SemanticStaticFieldAccessValidator.IsValid(defaults, requireOwners: false), "legacy readers reject owner metadata");
        Check(SemanticStaticFieldAccessValidator.IsValid(legacy, requireOwners: false)
            && !Encoding.UTF8.GetString(SemanticSerializer.Serialize(legacy)).Contains("static_field_owner_type_id", StringComparison.Ordinal),
            "default compilation omits owner metadata byte-for-byte");
        var ordinaryFields = Project("public class Cache { public static int Shared; public const int Constant = 2; public int Value; public int Read() { return Value + Constant; } }");
        foreach (var operation in ordinaryFields.Methods.SelectMany(method => Operations(method.Root))
            .Where(operation => operation.Kind == "field_reference"))
        {
            var invalidMethod = ordinaryFields.Methods.First() with { Root = operation with { StaticFieldOwnerTypeId = "type:global::Cache" } };
            Check(!SemanticStaticInitializationValidator.IsValid(ordinaryFields with { Methods = new[] { invalidMethod } }),
                "instance and constant fields cannot claim static storage");
        }
        foreach (string key in new[] { "base_schema_version", "base_semantic_version", "types" })
        {
            JsonNode node = JsonNode.Parse(SemanticSerializer.Serialize(baseline))!;
            node["static_initialization"]!.AsObject().Remove(key);
            bool rejected = false;
            try { rejected = !SemanticStaticInitializationValidator.IsValid(SemanticSerializer.Deserialize(Encoding.UTF8.GetBytes(node.ToJsonString()))); }
            catch (InvalidDataException) { rejected = true; }
            Check(rejected, "missing required property " + key);
        }
        DirectoryInfo? root = new(AppContext.BaseDirectory);
        const string fixtureId = "Fixtures/Phase66/AwaitMemberAssignment.cs";
        while (root is not null && !File.Exists(Path.Combine(root.FullName, fixtureId))) root = root.Parent;
        Check(root is not null, "original C10 fixture exists");
        string fixtureSource = File.ReadAllText(Path.Combine(root!.FullName, fixtureId));
        var original = SemanticAsyncMemberAssignmentTests.AnalyzeSource(fixtureSource, fixtureId);
        var combined = SemanticAsyncMemberAssignmentTests.AnalyzeSource(fixtureSource, fixtureId, enableStaticInitialization: true);
        Check(SemanticStaticInitializationValidator.IsValid(combined), "original C10 static initialization plan");
        foreach (string surface in new[] { "methods", "control_flow_graphs", "async_methods", "static_initialization" })
            RejectOwnerMutation(combined, surface);
        var throwing = Project("public class Cache { public static int Value; static Cache() { Value = 1; throw new System.InvalidOperationException(); } }");
        RejectOwnerMutation(throwing, "exception_flows");
        var storage = combined.StaticInitialization!;
        Check(storage.BaseSchemaVersion == 47 && storage.BaseSemanticVersion == "1.56", "C10 member assignment profile preserved");
        var execution = combined with { SchemaVersion = storage.BaseSchemaVersion,
            SemanticVersion = storage.BaseSemanticVersion, StaticInitialization = null };
        Check(SemanticAsyncMemberAssignmentValidator.IsValid(execution) && SemanticAsyncInvocationValidator.IsValid(execution)
            && execution.AsyncMethods.SelectMany(method => method.Segments).Count(segment => segment.AwaitSite?.MemberAssignment is not null) == 9,
            "all nine original member await sites preserve their contracts");
        JsonNode stripped = JsonNode.Parse(SemanticSerializer.Serialize(execution))!;
        void StripOwners(JsonNode? node)
        {
            if (node is JsonObject obj)
            {
                obj.Remove("static_field_owner_type_id");
                foreach (var property in obj) StripOwners(property.Value);
            }
            else if (node is JsonArray array) foreach (var child in array) StripOwners(child);
        }
        StripOwners(stripped);
        Check(execution.Source.Sha256 == original.Source.Sha256
            && SemanticSerializer.Serialize(SemanticSerializer.Deserialize(Encoding.UTF8.GetBytes(stripped.ToJsonString())))
                .SequenceEqual(SemanticSerializer.Serialize(original)),
            "only the versioned static owner annotations extend the original C10 execution profile");
        return count;

        void RejectOwnerMutation(SemanticDocument document, string surface)
        {
            JsonNode node = JsonNode.Parse(SemanticSerializer.Serialize(document))!;
            var access = Objects(node[surface]).First(item => item.ContainsKey("static_field_owner_type_id"));
            access.Remove("static_field_owner_type_id");
            Check(!SemanticStaticInitializationValidator.IsValid(SemanticSerializer.Deserialize(Encoding.UTF8.GetBytes(node.ToJsonString()))),
                "missing owner on " + surface + " must not hide behind other operation copies");
        }
    }

    private static IEnumerable<SemanticOperation> Operations(SemanticOperation operation)
    {
        yield return operation;
        foreach (var child in operation.Children)
            foreach (var nested in Operations(child)) yield return nested;
    }

    private static IEnumerable<JsonObject> Objects(JsonNode? node)
    {
        if (node is JsonObject obj)
        {
            yield return obj;
            foreach (var property in obj)
                foreach (var nested in Objects(property.Value)) yield return nested;
        }
        else if (node is JsonArray array)
            foreach (var child in array)
                foreach (var nested in Objects(child)) yield return nested;
    }
}
