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
            Check(document.SchemaVersion == 48 && document.SemanticVersion == "1.57", "outer identity");
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
        var type = plan.Types.Single();
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
        Check(constants.StaticInitialization is null && constants.SchemaVersion != 48, "constant-only source needs no initializer contract");
        var legacy = SemanticAnalyzer.Analyze(source, "Scripts/Static.cs", baseline.Source.Sha256);
        Check(legacy.StaticInitialization is null && !JsonNode.Parse(SemanticSerializer.Serialize(legacy))!.AsObject().ContainsKey("static_initialization"),
            "default API retains old serialized shape");

        SemanticDocument With(SemanticStaticTypeInitialization value) => baseline with { StaticInitialization = plan with { Types = new[] { value } } };
        SemanticDocument Field(SemanticStaticFieldInitialization value) => With(type with { Fields = new[] { value }.Concat(type.Fields.Skip(1)).ToArray() });
        foreach (SemanticDocument invalid in new[] {
            baseline with { SchemaVersion = plan.BaseSchemaVersion, SemanticVersion = plan.BaseSemanticVersion },
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
        var storage = combined.StaticInitialization!;
        Check(storage.BaseSchemaVersion == 47 && storage.BaseSemanticVersion == "1.56", "C10 member assignment profile preserved");
        var execution = combined with { SchemaVersion = storage.BaseSchemaVersion,
            SemanticVersion = storage.BaseSemanticVersion, StaticInitialization = null };
        Check(SemanticAsyncMemberAssignmentValidator.IsValid(execution) && SemanticAsyncInvocationValidator.IsValid(execution)
            && execution.AsyncMethods.SelectMany(method => method.Segments).Count(segment => segment.AwaitSite?.MemberAssignment is not null) == 9,
            "all nine original member await sites preserve their contracts");
        Check(execution.Source.Sha256 == original.Source.Sha256
            && SemanticSerializer.Serialize(execution).SequenceEqual(SemanticSerializer.Serialize(original)),
            "static plan leaves the original C10 execution profile unchanged");
        return count;
    }
}
