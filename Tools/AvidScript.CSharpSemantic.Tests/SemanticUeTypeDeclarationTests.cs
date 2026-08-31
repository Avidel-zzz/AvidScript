using System;
using System.IO;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpSemantic;

internal static class SemanticUeTypeDeclarationTests
{
    public static int Run()
    {
        ActorDeclarationsPreserveStableReflectionContract();
        ComponentAndSubsystemKindsResolveFromCanonicalRoots();
        InvalidClassContractsFailClosed();
        SubsystemNetworkContractsFailClosed();
        InvalidMemberContractsFailClosed();
        SpoofAttributesDoNotCreateUeTypes();
        SerializationIsDeterministic();
        SharedContractValidatorRejectsForgedIdentifiers();
        SharedSerializerReadsStrictArtifacts();
        return 9;
    }

    private static void ActorDeclarationsPreserveStableReflectionContract()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            [UClass(Name = "Hero", Blueprintable = true, BlueprintType = true)]
            public partial class Hero : AvidActor
            {
                [UProperty(EditAnywhere = true, BlueprintReadWrite = true, ReplicatedUsing = nameof(OnRepHealth), Category = "Combat")]
                public float Health { get; set; } = 100.0f;

                [UFunction(BlueprintCallable = true, Category = "Combat")]
                public virtual void ApplyDamage(float amount)
                {
                    Health -= amount;
                }

                [UFunction]
                private void OnRepHealth()
                {
                }

                protected override void BeginPlay()
                {
                }
            }

            [UClass]
            public partial class EliteHero : Hero
            {
                [UFunction(BlueprintCallable = true)]
                public override void ApplyDamage(float amount)
                {
                    base.ApplyDamage(amount * 0.5f);
                }
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/Hero.cs");
        SemanticUeTypeDeclaration hero = FindType(document, "global::Game.Hero");
        SemanticUeTypeDeclaration elite = FindType(document, "global::Game.EliteHero");

        Assert(document.Succeeded, "valid script-defined Actor inheritance should analyze successfully");
        Assert(document.SchemaVersion == 19 && document.SemanticVersion == "1.21",
            "UE type declarations should publish schema 19 / semantic 1.21");
        Assert(hero.EngineName == "Hero" && hero.Kind == "actor",
            "Actor declarations should preserve the stable UE reflection name and actor kind");
        Assert(hero.Flags.SequenceEqual(new[] { "blueprintable", "blueprint_type" }),
            "class flags should use canonical deterministic ordering");
        Assert(elite.Kind == "actor" && elite.BaseTypeId == hero.TypeId,
            "script inheritance should preserve the immediate script base identity and root kind");

        SemanticUePropertyDeclaration health = hero.Properties.Single();
        Assert(health.Name == "Health"
            && health.TypeId == "type:float32"
            && health.Category == "Combat"
            && health.ReplicatedUsing == "OnRepHealth"
            && health.Initializer == new SemanticUePropertyInitializer("float32", "100")
            && health.Flags.SequenceEqual(new[]
            {
                "edit_anywhere",
                "blueprint_read_write",
                "replicated",
            }),
            "UProperty should preserve normalized access, replication, category and initializer metadata");
        SemanticUeFunctionDeclaration applyDamage = hero.Functions.Single(function =>
            function.Name == "ApplyDamage");
        SemanticUeFunctionDeclaration beginPlay = hero.Functions.Single(function =>
            function.Name == "BeginPlay");
        SemanticUeFunctionDeclaration overrideFunction = elite.Functions.Single();
        Assert(applyDamage.Flags.SequenceEqual(new[] { "blueprint_callable" })
            && applyDamage.Category == "Combat",
            "UFunction should preserve callable and category metadata");
        Assert(beginPlay.Flags.SequenceEqual(new[] { "lifecycle", "override" }),
            "canonical lifecycle overrides should enter the UE function contract without a redundant attribute");
        Assert(overrideFunction.Flags.SequenceEqual(new[] { "blueprint_callable", "override" }),
            "script override identity should be explicit in the UE declaration contract");
        string applyDamageExport = SemanticUeTypeRuntimeContract.GetFunctionExportName(
            applyDamage.MethodSymbolId);
        Assert(applyDamageExport.StartsWith("avid_ue_", StringComparison.Ordinal)
            && applyDamageExport.Length == 40
            && document.Reachability!.RootCallableIds.Contains(applyDamage.MethodSymbolId),
            "script UE functions should publish one canonical WASM export identity and become reachability roots");
    }

    private static void ComponentAndSubsystemKindsResolveFromCanonicalRoots()
    {
        const string source = """
            using AvidScript;
            namespace Game;

            [UClass]
            public partial class AbilityComponent : AvidActorComponent { }

            [UClass]
            public partial class EncounterWorld : AvidWorldSubsystem { }

            [UClass]
            public partial class ProfileStore : AvidGameInstanceSubsystem { }
            """;

        SemanticDocument document = Analyze(source, "Scripts/ServiceTypes.cs");

        Assert(document.Succeeded, "canonical Component and Subsystem roots should be supported");
        Assert(FindType(document, "global::Game.AbilityComponent").Kind == "actor_component",
            "ActorComponent root should project actor_component");
        Assert(FindType(document, "global::Game.EncounterWorld").Kind == "world_subsystem",
            "WorldSubsystem root should project world_subsystem");
        Assert(FindType(document, "global::Game.ProfileStore").Kind == "game_instance_subsystem",
            "GameInstanceSubsystem root should project game_instance_subsystem");
        Assert(document.UeTypeDeclarations.Select(type => type.SymbolId).SequenceEqual(
            document.UeTypeDeclarations.Select(type => type.SymbolId).OrderBy(id => id, StringComparer.Ordinal)),
            "UE type declarations should be ordered by stable symbol identity");
    }

    private static void InvalidClassContractsFailClosed()
    {
        const string source = """
            using AvidScript;
            namespace Game;

            [UClass(Name = "Shared")]
            public class NotPartial : AvidActor { }

            [UClass(Name = "Shared")]
            public partial class DuplicateName : AvidActor { }

            [UClass]
            public partial class InvalidBase { }
            """;

        SemanticDocument document = Analyze(source, "Scripts/InvalidTypes.cs");

        Assert(!document.Succeeded, "invalid script-defined classes should fail semantic analysis");
        Assert(document.Diagnostics.Any(diagnostic => diagnostic.Code == "ASUE1002"),
            "non-partial UE script classes should use ASUE1002");
        Assert(document.Diagnostics.Any(diagnostic => diagnostic.Code == "ASUE1003"),
            "classes outside canonical UE roots should use ASUE1003");
        Assert(document.Diagnostics.Any(diagnostic => diagnostic.Code == "ASUE1004"),
            "duplicate engine names should use ASUE1004");
    }

    private static void SubsystemNetworkContractsFailClosed()
    {
        const string source = """
            using AvidScript;
            namespace Game;

            [UClass]
            public partial class InvalidNetworkSubsystem : AvidWorldSubsystem
            {
                [UProperty(ReplicatedUsing = nameof(OnRepValue))]
                public int Value { get; set; }

                [UFunction]
                private void OnRepValue() { }

                [UFunction(Server = true, Reliable = true)]
                public void ServerSubmitValue(int value) { }
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/InvalidNetworkSubsystem.cs");

        Assert(!document.Succeeded, "Subsystem network contracts should fail semantic analysis");
        Assert(document.Diagnostics.Any(diagnostic => diagnostic.Code == "ASUE1005"),
            "Subsystem replication and RPC declarations should use ASUE1005");
    }

    private static void InvalidMemberContractsFailClosed()
    {
        const string source = """
            using AvidScript;
            namespace Game;

            [UClass]
            public partial class InvalidMembers : AvidActor
            {
                [UProperty(EditAnywhere = true, VisibleAnywhere = true, BlueprintReadOnly = true, BlueprintReadWrite = true)]
                public int Conflicting { get; set; }

                [UProperty(ReplicatedUsing = "MissingNotify")]
                public int MissingNotify { get; set; }

                [UProperty]
                public int NonConstant { get; set; } = System.Environment.TickCount;

                [UFunction(Server = true, Client = true, Reliable = true, Unreliable = true)]
                public void InvalidRpc() { }

                [UProperty]
                public int RpcValue { get; set; }

                [UFunction(Server = true, Reliable = true)]
                public void ShadowedRpcParameter(int rpcValue) { }

                [UFunction]
                public static void StaticFunction() { }
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/InvalidMembers.cs");

        Assert(!document.Succeeded, "invalid UProperty/UFunction contracts should fail semantic analysis");
        Assert(document.Diagnostics.Any(diagnostic => diagnostic.Code == "ASUE1103"),
            "conflicting property flags should use ASUE1103");
        Assert(document.Diagnostics.Any(diagnostic => diagnostic.Code == "ASUE1104"),
            "invalid RepNotify should use ASUE1104");
        Assert(document.Diagnostics.Any(diagnostic => diagnostic.Code == "ASUE1105"),
            "non-constant property initializers should fail closed with ASUE1105");
        Assert(document.Diagnostics.Any(diagnostic => diagnostic.Code == "ASUE1202"),
            "static UFunctions should use ASUE1202");
        Assert(document.Diagnostics.Any(diagnostic => diagnostic.Code == "ASUE1203"),
            "conflicting network flags should use ASUE1203");
        Assert(document.Diagnostics.Any(diagnostic => diagnostic.Code == "ASUE1204"),
            "UHT case-insensitive parameter shadowing should use ASUE1204");
    }

    private static void SpoofAttributesDoNotCreateUeTypes()
    {
        const string source = """
            using System;
            using AvidScript;

            namespace Spoof
            {
                public sealed class UClassAttribute : Attribute { }
            }

            namespace Game
            {
                [Spoof.UClass]
                public partial class Spoofed : AvidActor { }
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/SpoofedType.cs");

        Assert(document.Succeeded, "a non-canonical attribute should remain ordinary C# metadata");
        Assert(document.UeTypeDeclarations.Count == 0,
            "same-name spoof attributes must not create UE type declarations");
    }

    private static void SerializationIsDeterministic()
    {
        const string source = """
            using AvidScript;
            [UClass]
            public partial class DeterministicActor : AvidActor
            {
                [UFunction(BlueprintPure = true)]
                public int Read() => 42;
            }
            """;

        SemanticDocument first = Analyze(source, "Scripts/DeterministicActor.cs");
        SemanticDocument second = Analyze(source, "Scripts/DeterministicActor.cs");
        byte[] firstBytes = SemanticSerializer.Serialize(first);
        byte[] secondBytes = SemanticSerializer.Serialize(second);

        Assert(firstBytes.SequenceEqual(secondBytes),
            "UE type declaration serialization should be byte deterministic");
        string json = System.Text.Encoding.UTF8.GetString(firstBytes);
        Assert(json.Contains("\"ue_type_declarations\"", StringComparison.Ordinal)
            && json.IndexOf("\"ue_type_declarations\"", StringComparison.Ordinal)
                < json.IndexOf("\"diagnostics\"", StringComparison.Ordinal),
            "UE type declarations should occupy the versioned semantic field before diagnostics");
    }

    private static void SharedContractValidatorRejectsForgedIdentifiers()
    {
        const string source = """
            using AvidScript;
            [UClass]
            public partial class ContractActor : AvidActor { }
            """;
        SemanticDocument document = Analyze(source, "Scripts/ContractActor.cs");
        SemanticUeTypeDeclaration declaration = document.UeTypeDeclarations.Single();
        SemanticDocument forged = document with
        {
            UeTypeDeclarations = new[] { declaration with { EngineName = "Bad-Name" } },
        };
        SemanticDocument cyclic = document with
        {
            UeTypeDeclarations = new[] { declaration with { BaseTypeId = declaration.TypeId } },
        };

        Assert(SemanticUeTypeContractValidator.TryValidate(document, out string validError)
            && validError.Length == 0,
            "the shared UE type contract validator should accept canonical semantic output");
        Assert(!SemanticUeTypeContractValidator.TryValidate(forged, out string forgedError)
            && forgedError.Contains("malformed", StringComparison.Ordinal),
            "the shared UE type contract validator should reject forged non-identifier engine names");
        Assert(!SemanticUeTypeContractValidator.TryValidate(cyclic, out string cycleError)
            && cycleError.Contains("cyclic", StringComparison.Ordinal),
            "the shared UE type contract validator should reject forged inheritance cycles");
    }

    private static void SharedSerializerReadsStrictArtifacts()
    {
        const string source = """
            using AvidScript;
            [UClass]
            public partial class SerializedActor : AvidActor { }
            """;
        SemanticDocument document = Analyze(source, "Scripts/SerializedActor.cs");
        SemanticDocument roundTrip = SemanticSerializer.Deserialize(SemanticSerializer.Serialize(document));

        Assert(roundTrip.SchemaVersion == document.SchemaVersion
            && roundTrip.Source.Sha256 == document.Source.Sha256
            && roundTrip.UeTypeDeclarations.Single().SymbolId == document.UeTypeDeclarations.Single().SymbolId,
            "the shared serializer should preserve strict semantic artifact identity");

        bool rejected = false;
        try
        {
            SemanticSerializer.Deserialize("not-json"u8);
        }
        catch (InvalidDataException)
        {
            rejected = true;
        }
        Assert(rejected, "the shared serializer should reject malformed semantic artifacts");
    }

    private static SemanticDocument Analyze(string source, string sourceId)
    {
        string hash = FrontendAnalyzer.Analyze(source, sourceId).Source.Sha256;
        return SemanticAnalyzer.Analyze(
            source,
            sourceId,
            hash,
            new[] { new SemanticReferenceSource(UeTypeFacade, "generated://AvidScript.UeTypes.cs") });
    }

    private static SemanticUeTypeDeclaration FindType(SemanticDocument document, string canonicalName)
    {
        string typeId = "type:" + canonicalName;
        return document.UeTypeDeclarations.Single(type => type.TypeId == typeId);
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }

    private const string UeTypeFacade = """
        using System;

        namespace AvidScript;

        [AttributeUsage(AttributeTargets.Class, Inherited = false, AllowMultiple = false)]
        public sealed class UClassAttribute : Attribute
        {
            public string Name { get; set; } = "";
            public bool Blueprintable { get; set; } = true;
            public bool BlueprintType { get; set; } = true;
            public bool Abstract { get; set; }
            public bool NotPlaceable { get; set; }
            public bool Transient { get; set; }
        }

        [AttributeUsage(AttributeTargets.Field | AttributeTargets.Property, Inherited = false, AllowMultiple = false)]
        public sealed class UPropertyAttribute : Attribute
        {
            public bool EditAnywhere { get; set; }
            public bool VisibleAnywhere { get; set; }
            public bool BlueprintReadOnly { get; set; }
            public bool BlueprintReadWrite { get; set; }
            public bool Replicated { get; set; }
            public string ReplicatedUsing { get; set; } = "";
            public bool SaveGame { get; set; }
            public bool Transient { get; set; }
            public string Category { get; set; } = "";
        }

        [AttributeUsage(AttributeTargets.Method, Inherited = false, AllowMultiple = false)]
        public sealed class UFunctionAttribute : Attribute
        {
            public bool BlueprintCallable { get; set; }
            public bool BlueprintPure { get; set; }
            public bool BlueprintNativeEvent { get; set; }
            public bool BlueprintImplementableEvent { get; set; }
            public bool Server { get; set; }
            public bool Client { get; set; }
            public bool NetMulticast { get; set; }
            public bool Reliable { get; set; }
            public bool Unreliable { get; set; }
            public string Category { get; set; } = "";
        }

        public abstract class AvidActor
        {
            protected virtual void BeginPlay() { }
            protected virtual void Tick(float deltaSeconds) { }
            protected virtual void EndPlay() { }
        }
        public abstract class AvidActorComponent
        {
            protected virtual void BeginPlay() { }
            protected virtual void Tick(float deltaSeconds) { }
            protected virtual void EndPlay() { }
        }
        public abstract class AvidWorldSubsystem
        {
            protected virtual void Initialize() { }
            protected virtual void Tick(float deltaSeconds) { }
            protected virtual void Deinitialize() { }
        }
        public abstract class AvidGameInstanceSubsystem
        {
            protected virtual void Initialize() { }
            protected virtual void Deinitialize() { }
        }
        """;
}
