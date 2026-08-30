using System;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestCompositeCapabilityTests
{
    private const string CompositeReferenceSource = """
        using System.Runtime.InteropServices;

        namespace AvidScript
        {
            public readonly struct UObject
            {
            }

            public readonly struct FAvidText
            {
                private readonly int Token;
                internal FAvidText(int token) { Token = token; }
            }

            public readonly struct FAvidSoftObject<T> where T : struct
            {
                private readonly int Token;
                internal FAvidSoftObject(int token) { Token = token; }
            }

            public readonly struct FAvidWeakObject<T> where T : struct
            {
                private readonly int Token;
                internal FAvidWeakObject(int token) { Token = token; }
            }

            public readonly struct FAvidArray<T>
            {
                private readonly int Token;
                internal FAvidArray(int token) { Token = token; }
            }

            public readonly struct FAvidSet<T>
            {
                private readonly int Token;
                internal FAvidSet(int token) { Token = token; }
            }

            public readonly struct FAvidMap<TKey, TValue>
            {
                private readonly int Token;
                internal FAvidMap(int token) { Token = token; }
            }

            internal static class Native
            {
                [DllImport("avidscript", EntryPoint = "avid_value_container_read")]
                internal static extern int Read(
                    FAvidArray<string> value,
                    int index,
                    int lane,
                    out string result);

                [DllImport("avidscript", EntryPoint = "avid_value_container_find")]
                internal static extern int Find(FAvidSet<int> value, in int input);

                [DllImport("avidscript", EntryPoint = "avid_value_container_upsert")]
                internal static extern int Upsert(
                    FAvidSet<int> value,
                    in int input,
                    int unused);

                [DllImport("avidscript", EntryPoint = "avid_value_container_remove")]
                internal static extern int Remove(FAvidSet<int> value, in int input);
            }
        }
        """;

    public static int Run()
    {
        CompositeWrappersLowerToOneCellNominalValues();
        CompositeContainerMutationImportsLowerToStableAbi();
        PrimarySourceCannotForgeWellShapedCompositeWrapper();
        MalformedCompositeWrapperFailsClosed();
        return 4;
    }

    private static void CompositeWrappersLowerToOneCellNominalValues()
    {
        const string source = """
            using System.Runtime.InteropServices;

            namespace AvidScript
            {
                public static class Script
                {
                    [UnmanagedCallersOnly(EntryPoint = "echo_text")]
                    public static FAvidText EchoText(FAvidText value) => value;

                    [UnmanagedCallersOnly(EntryPoint = "echo_soft")]
                    public static FAvidSoftObject<UObject> EchoSoft(
                        FAvidSoftObject<UObject> value) => value;

                    [UnmanagedCallersOnly(EntryPoint = "echo_weak")]
                    public static FAvidWeakObject<UObject> EchoWeak(
                        FAvidWeakObject<UObject> value) => value;

                    [UnmanagedCallersOnly(EntryPoint = "echo_array")]
                    public static FAvidArray<int> EchoArray(FAvidArray<int> value) => value;

                    [UnmanagedCallersOnly(EntryPoint = "echo_set")]
                    public static FAvidSet<int> EchoSet(FAvidSet<int> value) => value;

                    [UnmanagedCallersOnly(EntryPoint = "echo_map")]
                    public static FAvidMap<int, int> EchoMap(
                        FAvidMap<int, int> value) => value;
                }
            }
            """;

        GuestModule module = Lower(source, includeCompositeReferenceSource: true);
        GuestType[] compositeTypes = module.Types
            .Where(type => type.Kind == "composite_ref")
            .ToArray();
        Assert(compositeTypes.Length >= 6
            && compositeTypes.All(type =>
                type.Storage == "i32"
                && type.Size == 4
                && type.Alignment == 4
                && type.Fields.Count == 0),
            "text, object, and recursive container wrappers should share the nominal one-cell ABI");
        Assert(module.Functions.Count(function =>
                function.Parameters.Count == 1
                && function.ReturnTypeId == function.Parameters[0].TypeId
                && module.Types.Any(type =>
                    type.Id == function.ReturnTypeId
                    && type.Kind == "composite_ref")) == 6,
            "all six exported identity functions should retain their nominal composite type");

        WasmCompilationResult wasm = WasmModuleCompiler.Compile(module);
        WasmArtifactInfo artifact = WasmArtifactInspector.Inspect(wasm.Bytes);
        Assert(wasm.Succeeded
            && artifact.Exports.Count(exported =>
                exported.Name is "echo_text" or "echo_soft" or "echo_weak"
                    or "echo_array" or "echo_set" or "echo_map") == 6,
            "composite reference functions should compile to executable WASM exports");
    }

    private static void CompositeContainerMutationImportsLowerToStableAbi()
    {
        const string source = """
            using System.Runtime.InteropServices;

            namespace AvidScript
            {
                public static class Script
                {
                    [UnmanagedCallersOnly(EntryPoint = "read_string_array")]
                    public static int ReadString(FAvidArray<string> value)
                    {
                        string result;
                        return Native.Read(value, 0, 0, out result);
                    }

                    [UnmanagedCallersOnly(EntryPoint = "mutate_set")]
                    public static int Mutate(FAvidSet<int> value, int input)
                    {
                        int found = Native.Find(value, in input);
                        int upserted = Native.Upsert(value, in input, 0);
                        int removed = Native.Remove(value, in input);
                        return found + upserted + removed;
                    }
                }
            }
            """;

        GuestModule module = Lower(source, includeCompositeReferenceSource: true);
        string[] expectedImports =
        {
            "avid_value_container_read",
            "avid_value_container_find",
            "avid_value_container_upsert",
            "avid_value_container_remove"
        };
        Assert(expectedImports.All(name => module.Imports.Count(import =>
                import.Module == "avidscript" && import.Name == name) == 1),
            "container mutation should lower to one stable import per operation");
        Assert(module.Imports.All(import => import.ReturnTypeId == "type:int32")
            && module.Imports.Single(import =>
                import.Name == "avid_value_container_read").ParameterTypeIds.Count == 4
            && module.Imports.Single(import =>
                import.Name == "avid_value_container_find").ParameterTypeIds.Count == 2
            && module.Imports.Single(import =>
                import.Name == "avid_value_container_upsert").ParameterTypeIds.Count == 3
            && module.Imports.Single(import =>
                import.Name == "avid_value_container_remove").ParameterTypeIds.Count == 2,
            "container mutation imports should retain their frozen i32 ABI");
        string readStringFunctionId = module.Exports.Single(exported =>
            exported.Name == "read_string_array").FunctionId;
        GuestFunction readString = module.Functions.Single(function =>
            function.Id == readStringFunctionId);
        Assert(readString.Blocks.SelectMany(block => block.Instructions).Any(instruction =>
                instruction.Op == "address_of"),
            "string container reads should allocate one addressable i32 result slot");

        WasmCompilationResult wasm = WasmModuleCompiler.Compile(module);
        WasmArtifactInfo artifact = WasmArtifactInspector.Inspect(wasm.Bytes);
        Assert(wasm.Succeeded
            && artifact.Exports.Count(exported =>
                exported.Name is "mutate_set" or "read_string_array") == 2,
            "container mutation and string reads should compile to executable WASM exports");
    }

    private static void PrimarySourceCannotForgeWellShapedCompositeWrapper()
    {
        const string source = """
            using System.Runtime.InteropServices;

            namespace AvidScript
            {
                public readonly struct FAvidText
                {
                    private readonly int Token;
                    internal FAvidText(int token) { Token = token; }
                }

                public static class Script
                {
                    [UnmanagedCallersOnly(EntryPoint = "forged_text")]
                    public static FAvidText Echo(FAvidText value) => value;
                }
            }
            """;

        SemanticDocument semantic = Analyze(source, includeCompositeReferenceSource: false);
        CSharpGuestLoweringResult lowered = CSharpGuestLowerer.Lower(
            semantic,
            new string('c', 64));
        Assert(!lowered.Succeeded
            && lowered.Module is null
            && lowered.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCG1003"),
            "a primary-source wrapper must not forge an executable-reference capability: "
                + string.Join(" | ", lowered.Diagnostics.Select(item => $"{item.Code}:{item.Message}")));
    }

    private static void MalformedCompositeWrapperFailsClosed()
    {
        const string source = """
            using System.Runtime.InteropServices;

            namespace AvidScript
            {
                public readonly struct FAvidText
                {
                    private readonly long Token;
                    internal FAvidText(long token) { Token = token; }
                }

                public static class Script
                {
                    [UnmanagedCallersOnly(EntryPoint = "bad_text")]
                    public static FAvidText Echo(FAvidText value) => value;
                }
            }
            """;

        SemanticDocument semantic = Analyze(source, includeCompositeReferenceSource: false);
        CSharpGuestLoweringResult lowered = CSharpGuestLowerer.Lower(
            semantic,
            new string('c', 64));
        Assert(!lowered.Succeeded
            && lowered.Module is null
            && lowered.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCG1003"),
            "a forged composite wrapper should fail the generated nominal contract: "
                + string.Join(" | ", lowered.Diagnostics.Select(item => $"{item.Code}:{item.Message}")));
    }

    private static GuestModule Lower(string source, bool includeCompositeReferenceSource)
    {
        SemanticDocument semantic = Analyze(source, includeCompositeReferenceSource);
        CSharpGuestLoweringResult lowered = CSharpGuestLowerer.Lower(
            semantic,
            new string('c', 64));
        if (!lowered.Succeeded || lowered.Module is null)
        {
            throw new InvalidOperationException(string.Join(
                " | ",
                lowered.Diagnostics.Select(item => $"{item.Code}:{item.Message}")));
        }
        return lowered.Module;
    }

    private static SemanticDocument Analyze(
        string source,
        bool includeCompositeReferenceSource)
    {
        const string sourceId = "Scripts/CompositeCapability.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        SemanticDocument semantic = SemanticAnalyzer.Analyze(
            source,
            sourceId,
            frontend.Source.Sha256,
            includeCompositeReferenceSource
                ? new[]
                {
                    new SemanticReferenceSource(
                        CompositeReferenceSource,
                        "Generated/AvidScriptBindings.g.cs",
                        IsExecutable: true),
                }
                : Array.Empty<SemanticReferenceSource>());
        if (!frontend.Succeeded || !semantic.Succeeded)
        {
            throw new InvalidOperationException(string.Join(
                " | ",
                frontend.Diagnostics.Select(item => $"{item.Code}:{item.Message}")
                    .Concat(semantic.Diagnostics.Select(item => $"{item.Code}:{item.Message}"))));
        }
        return semantic;
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
