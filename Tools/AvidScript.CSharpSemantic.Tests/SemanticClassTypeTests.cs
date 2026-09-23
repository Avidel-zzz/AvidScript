using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json.Nodes;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpSemantic;

internal static class SemanticClassTypeTests
{
    public static int Run()
    {
        const string source = """
            using System;
            public interface IValue { int Read(); }
            public class Base { public virtual int Read() => 1; }
            public sealed class Derived : Base, IValue { public override int Read() => 2; }
            public class Plain { public int Value; public Plain Next; }
            public class Explicit { public Explicit() {} }
            public partial class Initial { public int First; }
            public partial class Initial { public int Second = 3; public int Auto { get; set; } = 4; }
            public class EventOwner { public event Action Changed; }
            public class Finalized { ~Finalized() {} }
            public class Generic<T> { public class Nested {} }
            public class Primary(int value) { public int Value = value; }
            public record Record(int Value);
            public static class Static { public static int Value = 7; }
            public class Constants { public const int Value = 1; }
            """;
        SemanticDocument document = Analyze(source);
        int count = 0;
        Check(document.SchemaVersion == 31 && document.SemanticVersion == "1.38"
            && SemanticClassContractValidator.IsValid(document), "class metadata must satisfy the versioned contract even if other syntax is unsupported: "
                + string.Join(" | ", document.ClassTypes));
        SemanticClassType plain = Find("Plain");
        Check(plain.IsSourceDeclared && plain.BaseTypeId == "type:object" && plain.HasImplicitDefaultConstructor
            && !plain.HasInstanceInitializers && !plain.HasImplicitInstanceStorage && !plain.HasVirtualMembers,
            "plain reference classes retain zero-initialization and constructor facts");
        Check(!Find("Explicit").HasImplicitDefaultConstructor, "an explicit parameterless constructor is not a default no-op constructor");
        SemanticClassType derived = Find("Derived");
        Check(derived.BaseTypeId == Find("Base").TypeId && derived.IsSealed && derived.HasVirtualMembers
            && derived.InterfaceTypeIds.SequenceEqual(new[] { "type:global::IValue" }), "base, interface and override facts must survive");
        Check(Find("Initial").HasInstanceInitializers && Find("Initial").HasImplicitInstanceStorage,
            "partial declarations and auto-property initializers must not disappear");
        Check(Find("EventOwner").HasImplicitInstanceStorage, "field-like events own compiler-generated storage");
        Check(Find("Finalized").HasFinalizer, "finalization is a distinct lifetime requirement");
        Check(Find("Generic<T>").IsGeneric && Find("Generic<T>.Nested").IsGeneric, "nested generic context remains explicit");
        Check(Find("Primary").HasPrimaryConstructor && !Find("Primary").HasImplicitDefaultConstructor,
            "primary constructors must not be mistaken for zero-initialization");
        Check(Find("Record").IsRecord && Find("Record").HasImplicitInstanceStorage, "records retain synthesized storage semantics");
        Check(Find("Static").IsStatic
            && Find("Static").HasStaticInitialization && !Find("Constants").HasStaticInitialization,
            "static initialization must be distinguished from constants");
        SemanticClassType root = document.ClassTypes.Single(item => item.TypeId == "type:object");
        Check(!root.IsSourceDeclared && root.BaseTypeId is null, "metadata classes are not guest source definitions");
        byte[] bytes = SemanticSerializer.Serialize(document);
        Check(bytes.SequenceEqual(SemanticSerializer.Serialize(SemanticSerializer.Deserialize(bytes)))
            && bytes.SequenceEqual(SemanticSerializer.Serialize(Analyze(source))), "class facts serialize deterministically");
        foreach (string property in JsonNode.Parse(bytes)!["class_types"]![0]!.AsObject().Select(pair => pair.Key).ToArray())
        {
            JsonNode json = JsonNode.Parse(bytes)!;
            json["class_types"]![0]!.AsObject().Remove(property);
            bool rejected = false;
            try { SemanticSerializer.Deserialize(Encoding.UTF8.GetBytes(json.ToJsonString())); }
            catch (InvalidDataException) { rejected = true; }
            Check(rejected, "missing class facts must not silently become false or null: " + property);
        }
        foreach (SemanticDocument malformed in new[]
        {
            document with { ClassTypes = null! },
            document with { ClassTypes = Array.Empty<SemanticClassType>() },
            document with { ClassTypes = document.ClassTypes.Append(plain).ToArray() },
            Replace(plain with { BaseTypeId = null }), Replace(plain with { BaseTypeId = "missing" }),
            Replace(plain with { BaseTypeId = plain.TypeId }), Replace(plain with { BaseTypeId = derived.TypeId }),
            Replace(plain with { InterfaceTypeIds = null! }), Replace(plain with { InterfaceTypeIds = new[] { plain.TypeId } }),
            Replace(derived with { InterfaceTypeIds = new[] { "type:global::IValue", "type:global::IValue" } }),
            Replace(plain with { IsStatic = true }), Replace(plain with { HasPrimaryConstructor = true }),
            document with { ClassTypes = document.ClassTypes.Select(item => item == plain ? item with { BaseTypeId = Find("Base").TypeId }
                : item.TypeId == Find("Base").TypeId ? item with { BaseTypeId = plain.TypeId } : item).ToArray() },
            document with { SchemaVersion = 23, SemanticVersion = "1.27" },
            document with { SchemaVersion = SemanticContract.CurrentSchemaVersion + 1 },
        }) Check(!SemanticClassContractValidator.IsValid(malformed), "invalid, cyclic, incomplete or downgraded class facts must fail closed");
        Check(SemanticClassContractValidator.IsValid(document with { SchemaVersion = 23, SemanticVersion = "1.27",
            ClassTypes = Array.Empty<SemanticClassType>() }), "legacy documents remain readable without claiming class facts");
        return count;

        SemanticClassType Find(string name) => document.ClassTypes.Single(item => item.TypeId == "type:global::" + name);
        SemanticDocument Replace(SemanticClassType replacement) => document with {
            ClassTypes = document.ClassTypes.Select(item => item.TypeId == replacement.TypeId ? replacement : item).ToArray() };
        void Check(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); count++; }
    }

    private static SemanticDocument Analyze(string source)
    {
        const string id = "Scripts/ClassContract.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, id);
        return SemanticAnalyzer.Analyze(source, id, frontend.Source.Sha256);
    }
}
