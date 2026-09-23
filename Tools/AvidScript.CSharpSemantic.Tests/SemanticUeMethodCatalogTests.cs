using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json.Nodes;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpSemantic;

internal static class SemanticUeMethodCatalogTests
{
    public static int Run()
    {
        const string source = """
            using AvidScript;
            public interface IRead { int Read(); }
            public interface IExtra : IRead { int Extra(); }
            [UClass] public partial class Root : AvidActor, IExtra {
                public virtual int Read() => 1;
                int IExtra.Extra() => 10;
                private double Mixed(ref int a, out long b, in float c) { b = a; return c; }
                private int Overloaded(int a) => a;
                private long Overloaded(long a) => a;
                [UFunction] public int PublicEntry() => 7;
            }
            [UClass] public partial class Derived : Root {
                public sealed override int Read() => 2;
            }
            [UClass] public partial class Hidden : Root {
                public new virtual int Read() => 3;
            }
            [UClass] public partial class Leaf : Hidden {
                public override int Read() => 4;
            }
            [UClass] public partial class Reimplemented : Root, IRead {
                int IRead.Read() => 5;
            }
            """;
        SemanticDocument document = Analyze(source);
        int count = 0;
        Check(document.Succeeded, string.Join(" | ", document.Diagnostics.Select(item => item.Message)));
        Check(document.SchemaVersion == 29 && document.SemanticVersion == "1.33", "catalog uses Semantic 29/1.33");
        Check(SemanticUeMethodCatalogValidator.IsValid(document), "Roslyn catalog validates");
        SemanticUeMethodCatalog catalog = document.UeMethodCatalog!;
        SemanticUeMethodEntry Method(string name) => catalog.Methods.Single(item => item.MethodSymbolId.Contains("global::" + name + "(", StringComparison.Ordinal));
        SemanticUeMethodEntry root = Method("Root.Read"), derived = Method("Derived.Read"), hidden = Method("Hidden.Read"), leaf = Method("Leaf.Read");
        Check(catalog.Types.Count == 5 && catalog.Interfaces.Count == 2, "all generated types and inherited interface contracts are represented");
        Check(root.SignatureId == derived.SignatureId && hidden.SignatureId == root.SignatureId, "same signature does not imply the same slot");
        Check(hidden.Dispatch.SlotMethodSymbolId != root.Dispatch.SlotMethodSymbolId, "new virtual gets a separate slot");
        Check(Resolve("Derived", root, "virtual") == derived, "base declaration resolves to concrete override");
        Check(Resolve("Derived", root, "direct") == root, "base/direct access retains the base body");
        Check(Resolve("Leaf", root, "virtual") == root && Resolve("Leaf", hidden, "virtual") == leaf, "hidden slots are independent through inheritance");
        Check(Resolve("Derived", Method("IRead.Read"), "interface") == derived, "inherited implicit interface implementation follows the virtual slot");
        Check(Resolve("Leaf", Method("IRead.Read"), "interface") == root, "new virtual does not silently reimplement inherited interface");
        SemanticUeMethodEntry extra = catalog.Methods.Single(item => item.Dispatch.ExplicitInterfaceMethodIds.Contains(Method("IExtra.Extra").MethodSymbolId));
        Check(Resolve("Derived", Method("IExtra.Extra"), "interface") == extra, "private explicit implementation is inherited");
        Check(Resolve("Reimplemented", Method("IRead.Read"), "interface")!.ContainingTypeId == "type:global::Reimplemented", "explicit reimplementation replaces inherited interface route");
        SemanticUeMethodEntry mixed = Method("Root.Mixed");
        Check(mixed.Accessibility == "private" && mixed.HasGuestBody && !mixed.IsReflected, "private Guest method is separate from UFUNCTION surface");
        Check(mixed.ReturnTypeId == "type:float64" && mixed.Parameters.Select(item => item.RefKind).SequenceEqual(new[] { "ref", "out", "in" })
            && mixed.Parameters.Select(item => item.TypeId).SequenceEqual(new[] { "type:int32", "type:int64", "type:float32" }), "complete parameter type/order/ref signature retained");
        Check(catalog.Methods.Count(item => item.IsReflected) == 1 && Method("Root.PublicEntry").IsReflected, "only reflected function receives UE exposure");
        Check(catalog.Methods.Where(item => item.MethodSymbolId.Contains("Root.Overloaded(", StringComparison.Ordinal)).Select(item => item.SignatureId).Distinct().Count() == 2,
            "overloads have different signatures");
        Check(catalog.Methods.Any(item => item.ContainingTypeId == "type:object" && !item.HasGuestBody), "external base methods are declarations, not Guest bodies");
        Check(Resolve("Root", derived, "direct") is null && Resolve("Derived", root, "unknown") is null, "incompatible receiver and unknown dispatch fail closed");
        byte[] bytes = SemanticSerializer.Serialize(document);
        Check(bytes.SequenceEqual(SemanticSerializer.Serialize(Analyze(source))) && bytes.SequenceEqual(SemanticSerializer.Serialize(SemanticSerializer.Deserialize(bytes))),
            "catalog serializes deterministically and round-trips");
        Check(!SemanticUeMethodCatalogValidator.IsValid(document with { UeMethodCatalog = null }), "new artifact cannot omit catalog");
        Check(!SemanticUeMethodCatalogValidator.IsValid(document with { SchemaVersion = 25, SemanticVersion = "1.29" }), "legacy version cannot smuggle new catalog");
        Check(SemanticUeMethodCatalogValidator.IsValid(document with { SchemaVersion = 25, SemanticVersion = "1.29", UeMethodCatalog = null }), "legacy artifacts do not invent catalog facts");
        Reject(catalog with { SchemaVersion = 2 });
        Reject(catalog with { Methods = catalog.Methods.Concat(new[] { root }).ToArray() });
        Reject(catalog with { Methods = catalog.Methods.Select(item => item == mixed ? item with { SignatureId = root.SignatureId } : item).ToArray() });
        Reject(catalog with { Methods = catalog.Methods.Select(item => item == mixed ? item with { HasGuestBody = false } : item).ToArray() });
        Reject(catalog with { Methods = catalog.Methods.Select(item => item == mixed ? item with { IsReflected = true } : item).ToArray() });
        SemanticUeMethodType derivedType = catalog.Types.Single(item => item.TypeId == "type:global::Derived");
        RejectType(derivedType with { VirtualSlots = derivedType.VirtualSlots.Select(item => item.SlotMethodSymbolId == root.MethodSymbolId
            ? item with { ImplementationMethodSymbolId = root.MethodSymbolId } : item).ToArray() });
        RejectType(derivedType with { InterfaceRoutes = Array.Empty<SemanticUeInterfaceRoute>() });
        RejectType(derivedType with { InterfaceTypeIds = Array.Empty<string>() });
        RejectType(derivedType with { InterfaceRoutes = derivedType.InterfaceRoutes.Select(item => item with { ImplementationMethodSymbolId = mixed.MethodSymbolId }).ToArray() });
        RejectType(derivedType with { InterfaceRoutes = derivedType.InterfaceRoutes.Select(item => item with { ImplementationMethodSymbolId = Method("Root.PublicEntry").MethodSymbolId, Kind = "direct" }).ToArray() });
        Reject(catalog with { Interfaces = catalog.Interfaces.Select(item => item with { BaseInterfaceTypeIds = new[] { item.TypeId } }).ToArray() });
        Reject(catalog with { Methods = catalog.Methods.Select(item => item == mixed ? item with { Parameters = null! } : item).ToArray() });
        JsonObject jsonCatalog = JsonNode.Parse(bytes)!["ue_method_catalog"]!.AsObject();
        foreach (string property in jsonCatalog["methods"]![0]!.AsObject().Select(pair => pair.Key).ToArray())
        {
            JsonNode json = JsonNode.Parse(bytes)!;
            json["ue_method_catalog"]!["methods"]![0]!.AsObject().Remove(property);
            bool rejected = false;
            try { SemanticSerializer.Deserialize(Encoding.UTF8.GetBytes(json.ToJsonString())); }
            catch (InvalidDataException) { rejected = true; }
            Check(rejected, "omitted method fact must not default: " + property);
        }
        SemanticDocument generic = Analyze("""
            using AvidScript;
            public interface IValue<T> { T Read(); }
            [UClass] public partial class Both : AvidActor, IValue<int>, IValue<long> {
                int IValue<int>.Read() => 1;
                long IValue<long>.Read() => 2;
                [UFunction] public int Entry() => 3;
            }
            """);
        Check(generic.Succeeded && SemanticUeMethodCatalogValidator.IsValid(generic), "closed generic interface catalog validates; succeeded=" + generic.Succeeded
            + "; classes=" + SemanticClassContractValidator.IsValid(generic) + "; dispatch=" + SemanticDispatchContractValidator.IsValid(generic)
            + "; diagnostics=" + string.Join(" | ", generic.Diagnostics.Select(item => item.Message)));
        SemanticUeMethodEntry[] contracts = generic.UeMethodCatalog!.Methods.Where(item => item.ContainingTypeId.Contains("IValue<", StringComparison.Ordinal)).ToArray();
        Check(contracts.Length == 2 && contracts.Select(item => item.MethodSymbolId).Distinct().Count() == 2
            && contracts.Select(item => item.DeclarationMethodSymbolId).Distinct().Count() == 1, "closed interface identities cannot collapse to OriginalDefinition");
        Check(contracts.All(item => SemanticUeMethodResolver.Resolve(generic, "type:global::Both", item.MethodSymbolId, "interface")?.ReturnTypeId == item.ReturnTypeId),
            "closed generic interface routes retain their concrete return type");
        SemanticDocument defaults = Analyze("""
            using AvidScript;
            public interface IDefault { int Read() => 6; }
            [UClass] public partial class DefaultActor : AvidActor, IDefault { [UFunction] public int Entry() => 3; }
            """);
        Check(defaults.Succeeded && SemanticUeMethodCatalogValidator.IsValid(defaults), "default interface method has a valid distinct route");
        SemanticUeInterfaceRoute defaultRoute = defaults.UeMethodCatalog!.Types.Single().InterfaceRoutes.Single();
        Check(defaultRoute.Kind == "default_interface"
            && SemanticUeMethodResolver.Resolve(defaults, "type:global::DefaultActor", defaultRoute.InterfaceMethodSymbolId, "interface")?.ContainingTypeId == "type:global::IDefault",
            "default implementation does not invent a class method");
        SemanticDocument propertyDocument = Analyze("""
            using AvidScript;
            public interface IProperty { int Value { get; set; } }
            [UClass] public partial class PropertyActor : AvidActor, IProperty {
                private static int backing;
                public int Value { get => backing; set => backing = value; }
                private ref readonly int Borrow() => ref backing;
                [UFunction] public int Entry() => backing;
            }
            """);
        Check(propertyDocument.Succeeded && SemanticUeMethodCatalogValidator.IsValid(propertyDocument), "interface accessors and ref-readonly return have complete metadata");
        Check(propertyDocument.UeMethodCatalog!.Types.Single().InterfaceRoutes.Count == 2
            && propertyDocument.UeMethodCatalog.Methods.Single(item => item.Name == "Borrow").ReturnRefKind == "ref_readonly", "getter/setter routes and reference return are distinct facts");
        foreach (string field in new[] { "ordinal", "type_id", "ref_kind" })
        {
            JsonNode json = JsonNode.Parse(bytes)!;
            JsonNode method = json["ue_method_catalog"]!["methods"]!.AsArray().Single(item => item!["method_symbol_id"]!.GetValue<string>() == mixed.MethodSymbolId)!;
            method["parameters"]![0]!.AsObject().Remove(field);
            bool rejected = false;
            try { SemanticSerializer.Deserialize(Encoding.UTF8.GetBytes(json.ToJsonString())); }
            catch (InvalidDataException) { rejected = true; }
            Check(rejected, "parameter signature fact must be explicit: " + field);
        }
        return count;

        SemanticUeMethodEntry? Resolve(string receiver, SemanticUeMethodEntry method, string kind) =>
            SemanticUeMethodResolver.Resolve(document, "type:global::" + receiver, method.MethodSymbolId, kind);
        void Reject(SemanticUeMethodCatalog malformed) => Check(!SemanticUeMethodCatalogValidator.IsValid(document with { UeMethodCatalog = malformed }), "malformed catalog rejects");
        void RejectType(SemanticUeMethodType malformed) => Reject(catalog with { Types = catalog.Types.Select(item => item.TypeId == malformed.TypeId ? malformed : item).ToArray() });
        void Check(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); count++; }
    }

    private static SemanticDocument Analyze(string source)
    {
        const string facade = """
            namespace AvidScript {
                [System.AttributeUsage(System.AttributeTargets.Class)] public sealed class UClassAttribute : System.Attribute { }
                [System.AttributeUsage(System.AttributeTargets.Method)] public sealed class UFunctionAttribute : System.Attribute { }
                public abstract class AvidActor { }
            }
            """;
        const string path = "Scripts/UeMethodCatalog.cs";
        return SemanticAnalyzer.Analyze(source, path, FrontendAnalyzer.Analyze(source, path).Source.Sha256,
            new[] { new SemanticReferenceSource(facade, "generated://UeMethodFacade.cs") });
    }
}
