using System;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpSemantic;

internal static class SemanticCallableTests
{
    public static int Run()
    {
        CallableParametersAndAbiAttributesAreProjected();
        ArrayAndEnumTypeShapesAreProjected();
        DuplicateExportNamesFailClosed();
        MissingExportEntryPointFailsClosed();
        AvidExportSupportsCapabilityParameters();
        MixedExportAttributesFailClosed();
        GameplayCallbacksAreProjectedAndRooted();
        InvalidGameplayCallbackShapesFailClosed();
        GameplayCallbacksAreScopedToTheScriptHost();
        GameplayCallbacksRequireConcreteNonGenericBodies();
        DataLaneMetadataIsProjectedForMethodsAndAccessors();
        BufferedWriteMetadataRequiresExecutableGeneratedExtern();
        InvalidDataLaneMetadataFailsClosed();
        return 13;
    }

    private static void AvidExportSupportsCapabilityParameters()
    {
        const string source = """
            using System;

            namespace AvidScript;

            [AttributeUsage(AttributeTargets.Method)]
            public sealed class AvidExportAttribute : Attribute
            {
                public AvidExportAttribute(string entryPoint) { }
            }

            public static class Script
            {
                [AvidExport("avid_on_event")]
                public static void OnEvent(int[] values, float logicalCalls) { }
            }
            """;
        const string sourceId = "Scripts/AvidExportArray.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);

        SemanticDocument document = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);

        SemanticCallable export = document.Callables.Single(callable =>
            callable.Export?.Name == "avid_on_event");
        Assert(document.Succeeded
            && export.Parameters.Select(parameter => parameter.TypeId)
                .SequenceEqual(new[] { "type:int32[]", "type:float32" }),
            "AvidExport should retain capability parameters without CLR unmanaged ABI restrictions");
    }

    private static void MixedExportAttributesFailClosed()
    {
        const string source = """
            using System;
            using System.Runtime.InteropServices;

            namespace AvidScript;

            [AttributeUsage(AttributeTargets.Method)]
            public sealed class AvidExportAttribute : Attribute
            {
                public AvidExportAttribute(string entryPoint) { }
            }

            public static class Script
            {
                [AvidExport("avid_on_tick")]
                [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
                public static void Tick(float deltaSeconds) { }
            }
            """;
        const string sourceId = "Scripts/MixedExportAttributes.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);

        SemanticDocument document = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);

        Assert(!document.Succeeded
            && document.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS5005"),
            "mixed export attributes should fail closed with ASCS5005");
    }

    private static void DataLaneMetadataIsProjectedForMethodsAndAccessors()
    {
        const string source = """
            using System;

            namespace AvidScript
            {
                [AttributeUsage(AttributeTargets.Method)]
                public sealed class AvidScriptDataLaneAttribute : Attribute
                {
                    public AvidScriptDataLaneAttribute(string optimizationClass, int bindingOrdinal) { }
                }

                public struct Target
                {
                    [AvidScriptDataLane("snapshot_read", 4)]
                    public int Snapshot() => 0;

                    public int Value
                    {
                        [AvidScriptDataLane("buffered_write", 9)]
                        set { }
                    }
                }
            }
            """;
        const string sourceId = "Scripts/DataLaneMetadata.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);

        SemanticDocument document = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);

        Assert(document.Succeeded, "valid data-lane metadata should analyze successfully");
        Assert(document.SchemaVersion == 14 && document.SemanticVersion == "1.14",
            "data-lane metadata should publish semantic schema v14 / version 1.14");
        SemanticCallable method = document.Callables.Single(callable =>
            callable.MethodSymbolId.Contains(".Snapshot():int32", StringComparison.Ordinal));
        Assert(method.Optimization == new SemanticCallableOptimization("snapshot_read", 4),
            "method data-lane metadata should retain its class and binding ordinal");
        SemanticCallable setter = document.Callables.Single(callable =>
            callable.AssociatedSymbolId is not null
            && callable.ReturnTypeId == "type:void"
            && callable.Parameters.Count == 1);
        Assert(setter.HasBody && setter.Import is null && setter.Optimization is null,
            "user-authored accessor bodies must not publish buffered-write fusion metadata");
    }

    private static void BufferedWriteMetadataRequiresExecutableGeneratedExtern()
    {
        const string source = "namespace Game { public static class Script { public static void Main() { } } }";
        const string generatedSource = """
            using System;
            using System.Runtime.InteropServices;

            namespace AvidScript
            {
                [AttributeUsage(AttributeTargets.Method)]
                internal sealed class AvidScriptDataLaneAttribute : Attribute
                {
                    internal AvidScriptDataLaneAttribute(string optimizationClass, int bindingOrdinal) { }
                }

                public readonly struct Target
                {
                    public readonly int Slot;
                    public readonly int Generation;
                }

                internal static class Generated
                {
                    [AvidScriptDataLane("buffered_write", 23)]
                    [DllImport("avidscript", EntryPoint = "avid_generated_property_set")]
                    internal static extern void Set(Target target, int value);
                }
            }
            """;
        const string sourceId = "Scripts/GeneratedExternMetadata.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);

        SemanticDocument document = SemanticAnalyzer.Analyze(
            source,
            sourceId,
            frontend.Source.Sha256,
            new[]
            {
                new SemanticReferenceSource(
                    generatedSource,
                    "generated://AvidScript.Bindings.generated.cs",
                    IsExecutable: true),
            });

        SemanticCallable generatedSetter = document.Callables.Single(callable =>
            callable.Import?.Name == "avid_generated_property_set");
        Assert(document.Succeeded
            && !generatedSetter.HasBody
            && generatedSetter.Optimization == new SemanticCallableOptimization("buffered_write", 23),
            "executable generated extern setters should retain verified buffered-write metadata");
    }

    private static void InvalidDataLaneMetadataFailsClosed()
    {
        const string source = """
            using System;

            namespace AvidScript
            {
                [AttributeUsage(AttributeTargets.Method)]
                public sealed class AvidScriptDataLaneAttribute : Attribute
                {
                    public AvidScriptDataLaneAttribute(string optimizationClass, int bindingOrdinal) { }
                    public AvidScriptDataLaneAttribute(int bindingOrdinal, string optimizationClass) { }
                }
            }

            static class Script
            {
                [AvidScript.AvidScriptDataLane("unknown", 0)]
                public static void UnknownClass() { }

                [AvidScript.AvidScriptDataLane("fused_call", -1)]
                public static void NegativeOrdinal() { }

                [AvidScript.AvidScriptDataLane(3, "buffered_write")]
                public static void WrongConstructorShape() { }
            }
            """;
        const string sourceId = "Scripts/InvalidDataLaneMetadata.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);

        SemanticDocument document = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);

        SemanticDiagnostic[] diagnostics = document.Diagnostics
            .Where(diagnostic => diagnostic.Code == "ASCS5004")
            .ToArray();
        Assert(!document.Succeeded && diagnostics.Length == 3,
            "each invalid data-lane attribute should produce one stable ASCS5004 error");
        Assert(document.Callables
                .Where(callable => callable.MethodSymbolId.Contains("Script.", StringComparison.Ordinal))
                .All(callable => callable.Optimization is null),
            "invalid data-lane attributes must not publish partial optimization metadata");
        Assert(document.ControlFlowGraphs.Count == 0,
            "invalid data-lane metadata should fail closed before executable graphs are published");
    }

    private static void GameplayCallbacksAreScopedToTheScriptHost()
    {
        const string source = """
            using System.Runtime.InteropServices;

            namespace AvidScript;

            public readonly struct InputEvent { }

            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
                public static void Tick(float deltaSeconds) { }

                public static void OnInput(InputEvent input) { }
            }

            public static class UnrelatedHelper
            {
                public static int OnInput(int value) => value;
            }
            """;
        const string sourceId = "Scripts/ScopedNaturalGameplayCallbacks.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);

        SemanticDocument document = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);

        Assert(document.Succeeded && document.GameplayEventCallbacks.Count == 1,
            "reserved callback names outside the exported script host should be ignored");
        SemanticGameplayEventCallback callback = document.GameplayEventCallbacks.Single();
        Assert(callback.Name == "OnInput"
            && callback.MethodSymbolId.Contains("global::AvidScript.Script.OnInput", StringComparison.Ordinal),
            "the gameplay callback should belong to the exported script host");
    }

    private static void GameplayCallbacksRequireConcreteNonGenericBodies()
    {
        const string source = """
            using System.Runtime.InteropServices;

            namespace AvidScript;

            public readonly struct AActor { }
            public readonly struct FVector { }
            public readonly struct InputEvent { }

            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
                public static void Tick(float deltaSeconds) { }

                public static void OnHit<T>(AActor otherActor, FVector normalImpulse) { }

                [DllImport("env", EntryPoint = "host_input")]
                public static extern void OnInput(InputEvent input);
            }
            """;
        const string sourceId = "Scripts/BodylessNaturalGameplayCallbacks.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);

        SemanticDocument document = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);

        SemanticDiagnostic[] diagnostics = document.Diagnostics
            .Where(diagnostic => diagnostic.Code == "ASCS5107")
            .ToArray();
        Assert(!document.Succeeded && diagnostics.Length == 2,
            "generic and bodyless gameplay callbacks should fail during semantic analysis");
        foreach (string name in new[] { "OnHit", "OnInput" })
        {
            SemanticDiagnostic diagnostic = diagnostics.Single(item =>
                item.Span.Start == source.IndexOf(name, StringComparison.Ordinal));
            Assert(diagnostic.Span.Length == name.Length,
                $"ASCS5107 should identify the invalid {name} declaration");
        }
    }

    private static void CallableParametersAndAbiAttributesAreProjected()
    {
        const string source = """
            using System.Runtime.InteropServices;

            enum ResultCode : ushort { Ok = 1 }

            static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
                public static void Tick(float deltaSeconds) { }

                public static ResultCode Mix(ref int left, out float value, in long right)
                {
                    value = left + right;
                    return ResultCode.Ok;
                }

                [DllImport("env", EntryPoint = "host_call")]
                internal static extern int Host(int value, out float result);
            }
            """;
        const string sourceId = "Scripts/CallableAbi.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);

        SemanticDocument document = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);

        Assert(document.Succeeded, "callable ABI source should analyze successfully");
        SemanticCallable tick = document.Callables.Single(callable => callable.Export?.Name == "avid_on_tick");
        Assert(tick.MethodSymbolId.EndsWith(".Tick(float32):void", StringComparison.Ordinal),
            "export should retain stable method identity");
        Assert(tick.IsStatic && tick.HasBody && tick.ReturnTypeId == "type:void",
            "export should retain static/body/return metadata");
        Assert(tick.Parameters.Single().TypeId == "type:float32",
            "export parameter should retain canonical float type");

        SemanticCallable mix = document.Callables.Single(callable =>
            callable.MethodSymbolId.Contains(".Mix(ref int32,out float32,in int64)", StringComparison.Ordinal));
        Assert(mix.Parameters.Select(parameter => parameter.Ordinal).SequenceEqual(new[] { 0, 1, 2 }),
            "callable parameters should retain declaration order");
        Assert(mix.Parameters.Select(parameter => parameter.RefKind)
            .SequenceEqual(new[] { "ref", "out", "in" }),
            "callable parameters should retain stable ref kinds");
        Assert(mix.ReturnTypeId.EndsWith("ResultCode", StringComparison.Ordinal),
            "callable should retain enum return type");

        SemanticCallable host = document.Callables.Single(callable => callable.Import?.Name == "host_call");
        Assert(!host.HasBody && host.Import?.Module == "env",
            "DllImport should project a bodyless env import");
        Assert(host.Export is null, "import should not be projected as an export");
    }

    private static void ArrayAndEnumTypeShapesAreProjected()
    {
        const string source = """
            enum Direction : ushort { Left = 1, Right = 2 }
            sealed class Script
            {
                private Direction DirectionValue = Direction.Left;
                private int[] Values = new int[3];
            }
            """;
        const string sourceId = "Scripts/TypeShapes.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);

        SemanticDocument document = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);

        Assert(document.Succeeded, "array and enum source should analyze successfully");
        SemanticTypeShape array = document.TypeShapes.Single(shape => shape.ElementTypeId is not null);
        Assert(array.TypeId == "type:int32[]" && array.ElementTypeId == "type:int32",
            "array shape should retain canonical element type");
        SemanticTypeShape enumShape = document.TypeShapes.Single(shape => shape.EnumUnderlyingTypeId is not null);
        Assert(enumShape.TypeId.EndsWith("Direction", StringComparison.Ordinal) &&
            enumShape.EnumUnderlyingTypeId == "type:uint16",
            "enum shape should retain canonical underlying type");
    }

    private static void DuplicateExportNamesFailClosed()
    {
        const string source = """
            using System.Runtime.InteropServices;
            static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "avid_duplicate")]
                public static void First() { }

                [UnmanagedCallersOnly(EntryPoint = "avid_duplicate")]
                public static void Second() { }
            }
            """;
        const string sourceId = "Scripts/DuplicateExports.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);

        SemanticDocument document = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);

        Assert(!document.Succeeded, "duplicate export names should fail semantic analysis");
        Assert(document.Diagnostics.Single(diagnostic => diagnostic.Code == "ASCS5003").Severity == "error",
            "duplicate export names should use ASCS5003");
        Assert(document.ControlFlowGraphs.Count == 0,
            "callable ABI errors should clear every control-flow graph");
    }

    private static void MissingExportEntryPointFailsClosed()
    {
        const string source = """
            using System.Runtime.InteropServices;
            static class Script
            {
                [UnmanagedCallersOnly]
                public static void Tick() { }
            }
            """;
        const string sourceId = "Scripts/MissingExportEntryPoint.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);

        SemanticDocument document = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);

        Assert(!document.Succeeded, "missing export EntryPoint should fail semantic analysis");
        Assert(document.Diagnostics.Single(diagnostic => diagnostic.Code == "ASCS5002").Severity == "error",
            "missing export EntryPoint should use ASCS5002");
        Assert(document.ControlFlowGraphs.Count == 0,
            "invalid export metadata should clear every control-flow graph");
    }

    private static void GameplayCallbacksAreProjectedAndRooted()
    {
        const string source = """
            using System.Runtime.InteropServices;

            namespace AvidScript;

            public readonly struct AActor { }
            public readonly struct FVector { }
            public readonly struct InputEvent { }

            public static class Script
            {
                [DllImport("env", EntryPoint = "host_touch")]
                private static extern void HostTouch();

                public static void OnBeginOverlap(AActor otherActor, FVector location) => HostTouch();
                public static void OnEndOverlap(AActor otherActor, FVector location) { }
                public static void OnHit(AActor otherActor, FVector normalImpulse) { }
                public static void OnInput(InputEvent input) { }
            }
            """;
        const string sourceId = "Scripts/NaturalGameplayCallbacks.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);

        SemanticDocument document = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);

        Assert(document.Succeeded, "natural gameplay callbacks should analyze successfully");
        Assert(document.SchemaVersion == 14 && document.SemanticVersion == "1.14",
            "gameplay callback artifacts should use schema v14 / semantic version 1.14");
        Assert(document.GameplayEventCallbacks.Select(callback => callback.EventType)
            .SequenceEqual(new[] { 1, 2, 3, 4 }),
            "gameplay callbacks should use stable event-type order");
        foreach (SemanticGameplayEventCallback callback in document.GameplayEventCallbacks)
        {
            Assert(callback.Span.Start == source.IndexOf(callback.Name, StringComparison.Ordinal)
                && callback.Span.Length == callback.Name.Length,
                $"{callback.Name} should retain its exact identifier span");
            Assert(document.Reachability!.RootCallableIds.Contains(callback.MethodSymbolId),
                $"{callback.Name} should become a reachability root");
        }

        Assert(document.Reachability!.Mode == "entrypoint_roots",
            "natural callbacks should use the explicit entrypoint-root mode");
        Assert(document.Reachability.ReachableImports.Single().Name == "host_touch",
            "callback calls should retain generated host imports");
    }

    private static void InvalidGameplayCallbackShapesFailClosed()
    {
        const string source = """
            namespace AvidScript;

            public readonly struct AActor { }
            public readonly struct FVector { }
            public readonly struct InputEvent { }

            public sealed class Script
            {
                public void OnBeginOverlap(AActor otherActor, FVector location) { }
                public static int OnEndOverlap(AActor otherActor, FVector location) => 0;
                public static void OnHit(AActor otherActor) { }
                public static void OnInput(AActor input) { }
            }
            """;
        const string sourceId = "Scripts/InvalidNaturalGameplayCallbacks.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);

        SemanticDocument document = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);

        Assert(!document.Succeeded, "invalid natural callback shapes should fail semantic analysis");
        (string Code, string Name)[] expected =
        {
            ("ASCS5102", "OnBeginOverlap"),
            ("ASCS5103", "OnEndOverlap"),
            ("ASCS5104", "OnHit"),
            ("ASCS5105", "OnInput"),
        };
        foreach ((string code, string name) in expected)
        {
            SemanticDiagnostic diagnostic = document.Diagnostics.Single(item => item.Code == code);
            Assert(diagnostic.Span.Start == source.IndexOf(name, StringComparison.Ordinal)
                && diagnostic.Span.Length == name.Length,
                $"{code} should identify the invalid {name} declaration");
        }

        Assert(document.GameplayEventCallbacks.Count == 0,
            "invalid reserved callbacks should not publish partial descriptors");
        Assert(document.ControlFlowGraphs.Count == 0,
            "gameplay callback contract errors should clear every control-flow graph");
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
