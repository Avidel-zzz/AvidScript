using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpSemantic;
using AvidScript.UeTypeGenerator;

internal static class UeTypeGeneratorTests
{
    public static int Run()
    {
        GenerationIsByteDeterministicAndTopological();
        LifecycleKindsMapToNativeShells();
        UnsupportedTypesFailClosedBeforePublication();
        AtomicPublisherPreservesCacheHits();
        return 4;
    }

    private static void GenerationIsByteDeterministicAndTopological()
    {
        const string source = """
            using AvidScript;

            [UClass]
            public partial class Projectile : AvidActor
            {
                [UProperty(EditAnywhere = true, BlueprintReadWrite = true, ReplicatedUsing = nameof(OnRepDamage), Category = "Projectile")]
                public float Damage { get; set; } = 25.0f;

                [UFunction(BlueprintCallable = true, BlueprintNativeEvent = true, Category = "Projectile")]
                public virtual void Activate(float damageScale) { }

                [UFunction]
                private void OnRepDamage() { }

                protected override void BeginPlay() { }
            }

            [UClass]
            public partial class ExplosiveProjectile : Projectile
            {
                [UFunction(BlueprintCallable = true)]
                public override void Activate(float damageScale) { }
            }
            """;
        byte[] semantic = Analyze(source, "Scripts/Projectile.cs");
        UeTypeGenerationResult first = UeTypeShellGenerator.Generate(semantic, "AvidScriptGenerated", "5.8");
        UeTypeGenerationResult second = UeTypeShellGenerator.Generate(semantic, "AvidScriptGenerated", "5.8");

        Assert(first.Files.Keys.SequenceEqual(second.Files.Keys)
            && first.Files.All(pair => pair.Value.SequenceEqual(second.Files[pair.Key])),
            "identical semantic input should generate byte-identical source and manifest files");
        Assert(first.Manifest.Types.Select(type => type.TypeOrdinal)
            .SequenceEqual(Enumerable.Range(0, first.Manifest.Types.Count)),
            "type ordinals should be dense and deterministic");
        Assert(first.Manifest.Types.SelectMany(type => type.Properties.Cast<object>().Concat(type.Functions))
            .Count() == 5,
            "the manifest should retain reflected properties, functions and lifecycle members");
        IReadOnlyDictionary<string, SemanticUePropertyRuntimePlan> propertyPlans =
            SemanticUeTypeRuntimeContract.BuildPropertyPlans(
                SemanticSerializer.Deserialize(semantic)).ToDictionary(
                    plan => plan.PropertySymbolId,
                    StringComparer.Ordinal);
        UeFunctionManifestEntry derivedActivate = first.Manifest.Types
            .Single(type => type.EngineName == "ExplosiveProjectile")
            .Functions.Single(function => function.Name == "Activate");
        Assert(first.Manifest.SchemaVersion == 5
            && first.Manifest.GeneratorVersion == "1.5"
            && first.Manifest.Types.All(type =>
                type.ClassPath == $"/Script/AvidScriptGenerated.{type.EngineName}")
            && first.Manifest.Types.SelectMany(type => type.Functions).All(function =>
                function.ExportName == SemanticUeTypeRuntimeContract.GetFunctionExportName(
                    function.StableMemberId))
            && first.Manifest.Types.SelectMany(type => type.Properties).All(property =>
                property.GetterImportName == propertyPlans[property.StableMemberId].GetterImportName
                && property.SetterImportName == propertyPlans[property.StableMemberId].SetterImportName)
            && first.Manifest.Types.SelectMany(type => type.Properties)
                .Single(property => property.Name == "Damage").Initializer
                == new SemanticUePropertyInitializer("float32", "25")
            && derivedActivate.Flags.Contains("override")
            && derivedActivate.Flags.Contains("blueprint_native_event"),
            "manifest schema 5 should publish explicit class paths, runtime identities and initializers");

        string header = Text(first, "Public/AvidScriptGeneratedTypes.h");
        string sourceText = Text(first, "Private/AvidScriptGeneratedTypes.cpp");
        Assert(header.IndexOf("class AVIDSCRIPTGENERATED_API AProjectile", StringComparison.Ordinal)
            < header.IndexOf("class AVIDSCRIPTGENERATED_API AExplosiveProjectile", StringComparison.Ordinal),
            "native shell output should place script bases before derived classes");
        Assert(header.Contains("UPROPERTY(EditAnywhere, BlueprintReadWrite, ReplicatedUsing=OnRepDamage, Category=\"Projectile\")", StringComparison.Ordinal)
            && header.Contains("AProjectile();", StringComparison.Ordinal)
            && header.Contains("void Activate(float damageScale);", StringComparison.Ordinal)
            && header.Contains("virtual void Activate_Implementation(float damageScale);", StringComparison.Ordinal)
            && header.Contains("virtual void Activate_Implementation(float damageScale) override;", StringComparison.Ordinal),
            "native shell output should preserve normalized reflection and inherited implementation declarations");
        Assert(sourceText.Contains("FAvidScriptGeneratedTypeDispatcher::Invoke(", StringComparison.Ordinal)
            && sourceText.Contains("AProjectile::AProjectile()", StringComparison.Ordinal)
            && sourceText.Contains("AProjectile::Activate_Implementation(float damageScale)", StringComparison.Ordinal)
            && sourceText.Contains("AExplosiveProjectile::Activate_Implementation(float damageScale)", StringComparison.Ordinal)
            && sourceText.Contains("Damage = 25.0f;", StringComparison.Ordinal)
            && sourceText.Contains("DOREPLIFETIME(AProjectile, Damage);", StringComparison.Ordinal)
            && sourceText.Contains("AvidScript.GeneratedTypes.Reflection", StringComparison.Ordinal)
            && sourceText.Contains("FindFProperty<FProperty>", StringComparison.Ordinal),
            "generated bodies should use ordinal dispatch and emit a self-describing reflection probe");
    }

    private static void LifecycleKindsMapToNativeShells()
    {
        const string source = """
            using AvidScript;

            [UClass]
            public partial class MotionComponent : AvidActorComponent
            {
                protected override void Tick(float deltaSeconds) { }
            }

            [UClass]
            public partial class EncounterWorld : AvidWorldSubsystem
            {
                protected override void Initialize() { }
                protected override void Tick(float deltaSeconds) { }
            }

            [UClass]
            public partial class ProfileStore : AvidGameInstanceSubsystem
            {
                protected override void Deinitialize() { }
            }
            """;
        UeTypeGenerationResult result = UeTypeShellGenerator.Generate(
            Analyze(source, "Scripts/Services.cs"),
            "AvidScriptGenerated",
            "5.8");
        string header = Text(result, "Public/AvidScriptGeneratedTypes.h");
        string sourceText = Text(result, "Private/AvidScriptGeneratedTypes.cpp");

        Assert(header.Contains("class AVIDSCRIPTGENERATED_API UMotionComponent : public UActorComponent", StringComparison.Ordinal)
            && header.Contains("void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)", StringComparison.Ordinal),
            "component Tick should map to the native TickComponent lifecycle");
        Assert(header.Contains("class AVIDSCRIPTGENERATED_API UEncounterWorld : public UTickableWorldSubsystem", StringComparison.Ordinal)
            && header.Contains("void Initialize(FSubsystemCollectionBase& Collection)", StringComparison.Ordinal)
            && header.Contains("TStatId GetStatId() const", StringComparison.Ordinal),
            "world subsystem lifecycle should use the tickable native shell contract");
        Assert(header.Contains("class AVIDSCRIPTGENERATED_API UProfileStore : public UGameInstanceSubsystem", StringComparison.Ordinal),
            "game instance subsystem should use the native U prefix and base");
        Assert(header.Contains("virtual void BeginPlay() override;", StringComparison.Ordinal)
            && header.Contains("virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;", StringComparison.Ordinal)
            && header.Contains("virtual void Deinitialize() override;", StringComparison.Ordinal),
            "generated shells should always publish host activation and teardown hooks");
        int initializeStart = sourceText.IndexOf("void UEncounterWorld::Initialize", StringComparison.Ordinal);
        int initializeSuper = sourceText.IndexOf("Super::Initialize", initializeStart, StringComparison.Ordinal);
        int initializeHost = sourceText.IndexOf("BeginInstance", initializeStart, StringComparison.Ordinal);
        int initializeDispatch = sourceText.IndexOf("FAvidScriptGeneratedTypeDispatcher::Invoke", initializeStart, StringComparison.Ordinal);
        int deinitializeStart = sourceText.IndexOf("void UProfileStore::Deinitialize", StringComparison.Ordinal);
        int deinitializeDispatch = sourceText.IndexOf("FAvidScriptGeneratedTypeDispatcher::Invoke", deinitializeStart, StringComparison.Ordinal);
        int deinitializeHost = sourceText.IndexOf("EndInstance", deinitializeStart, StringComparison.Ordinal);
        int deinitializeSuper = sourceText.IndexOf("Super::Deinitialize", deinitializeStart, StringComparison.Ordinal);
        Assert(sourceText.Contains("AvidScriptGeneratedTypeRuntimeHost.h", StringComparison.Ordinal)
            && initializeStart >= 0
            && initializeSuper < initializeHost
            && initializeHost < initializeDispatch
            && deinitializeStart >= 0
            && deinitializeDispatch < deinitializeHost
            && deinitializeHost < deinitializeSuper,
            "native lifecycle should activate before canonical dispatch and tear down after it");
    }

    private static void UnsupportedTypesFailClosedBeforePublication()
    {
        const string source = """
            using System;
            using AvidScript;

            [UClass]
            public partial class InvalidShell : AvidActor
            {
                [UProperty]
                public DateTime Unsupported { get; set; }
            }
            """;
        bool rejected = false;
        try
        {
            UeTypeShellGenerator.Generate(
                Analyze(source, "Scripts/InvalidShell.cs"),
                "AvidScriptGenerated",
                "5.8");
        }
        catch (InvalidOperationException exception)
        {
            rejected = exception.Message.Contains("no deterministic UE shell mapping", StringComparison.Ordinal);
        }
        Assert(rejected, "types without a deterministic UE shell mapping should fail before publication");
    }

    private static void AtomicPublisherPreservesCacheHits()
    {
        const string source = """
            using AvidScript;
            [UClass]
            public partial class PublishedActor : AvidActor { }
            """;
        UeTypeGenerationResult result = UeTypeShellGenerator.Generate(
            Analyze(source, "Scripts/PublishedActor.cs"),
            "AvidScriptGenerated",
            "5.8");
        string root = Path.Combine(Path.GetTempPath(), "AvidScriptUeTypeGeneratorTests", Guid.NewGuid().ToString("N"));
        try
        {
            UeTypePublishResult first = UeTypeGenerationPublisher.Publish(root, result);
            DateTime manifestWrite = File.GetLastWriteTimeUtc(Path.Combine(root, UeTypeShellGenerator.ManifestPath));
            UeTypePublishResult second = UeTypeGenerationPublisher.Publish(root, result);

            Assert(first.ChangedFileCount == 3 && first.CacheHitFileCount == 0,
                "first publication should atomically write both sources and the manifest");
            Assert(second.ChangedFileCount == 0 && second.CacheHitFileCount == 3,
                "second publication should be a complete content-addressed cache hit");
            Assert(File.GetLastWriteTimeUtc(Path.Combine(root, UeTypeShellGenerator.ManifestPath)) == manifestWrite,
                "cache hits should not rewrite the manifest timestamp");
        }
        finally
        {
            if (Directory.Exists(root))
            {
                Directory.Delete(root, true);
            }
        }
    }

    private static byte[] Analyze(string source, string sourceId)
    {
        string hash = FrontendAnalyzer.Analyze(source, sourceId).Source.Sha256;
        SemanticDocument document = SemanticAnalyzer.Analyze(
            source,
            sourceId,
            hash,
            new[] { new SemanticReferenceSource(Facade, "generated://AvidScript.UeTypes.cs") });
        return SemanticSerializer.Serialize(document);
    }

    private static string Text(UeTypeGenerationResult result, string path)
    {
        return Encoding.UTF8.GetString(result.Files[path]);
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }

    private const string Facade = """
        using System;
        namespace AvidScript;

        [AttributeUsage(AttributeTargets.Class)]
        public sealed class UClassAttribute : Attribute
        {
            public string Name { get; set; } = "";
            public bool Blueprintable { get; set; } = true;
            public bool BlueprintType { get; set; } = true;
            public bool Abstract { get; set; }
            public bool NotPlaceable { get; set; }
            public bool Transient { get; set; }
        }
        [AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
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
        [AttributeUsage(AttributeTargets.Method)]
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
