using System;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestArrayCapabilityTests
{
    public static int Run()
    {
        ReachableArrayOperationsInjectCapabilityImports();
        ReadOnlyArrayRegionsDoNotInjectWriteRange();
        UnreachableArrayOperationsDoNotInjectCapabilityImports();
        return 3;
    }

    private static void ReachableArrayOperationsInjectCapabilityImports()
    {
        const string source = """
            using System.Runtime.InteropServices;

            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "guest_arrays")]
                public static int Main()
                {
                    int[] values = new[] { 3, 5, 8 };
                    values[0] = values[1];
                    return values.Length;
                }
            }
            """;
        GuestModule module = Lower(source);
        GuestInstruction[] instructions = module.Functions
            .SelectMany(function => function.Blocks)
            .SelectMany(block => block.Instructions)
            .ToArray();
        GuestImport[] arrayImports = module.Imports
            .Where(import => import.Module == GuestArrayCapabilityIntrinsics.Module)
            .ToArray();

        Assert(instructions.Count(instruction => instruction.Op == "array_region_load") == 1
            && instructions.Count(instruction => instruction.Op == "array_region_store") == 1
            && instructions.Count(instruction => instruction.Op == "array_length") == 1,
            "one-dimensional C# array reads and writes should lower to managed regions with typed Length");
        Assert(arrayImports.Select(import => import.Name).SequenceEqual(new[]
            {
                GuestArrayCapabilityIntrinsics.LengthImportName,
                GuestArrayCapabilityIntrinsics.ReadRangeImportName,
                GuestArrayCapabilityIntrinsics.WriteRangeImportName,
            }),
            "reachable region instructions should inject only the existing length and range imports");
        Assert(arrayImports[0].ParameterTypeIds.Count == 1
            && arrayImports.Skip(1).All(import => import.ParameterTypeIds.Count == 5)
            && arrayImports.All(import => import.ReturnTypeId == "type:int32"),
            "array capability imports should retain the frozen (i)i and (iiiii)i signatures");

        WasmCompilationResult wasm = WasmModuleCompiler.Compile(module);
        WasmArtifactInfo artifact = WasmArtifactInspector.Inspect(wasm.Bytes);
        Assert(wasm.Succeeded
            && artifact.Imports.Count(import => import.Module == "avidscript") == 3,
            "array capability Guest IR should compile to WASM with all static imports");
    }

    private static void ReadOnlyArrayRegionsDoNotInjectWriteRange()
    {
        const string source = """
            using System.Runtime.InteropServices;

            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "guest_array_read")]
                public static int Main()
                {
                    int[] values = new[] { 3, 5, 8 };
                    return values[1];
                }
            }
            """;
        GuestModule module = Lower(source);
        GuestImport[] arrayImports = module.Imports
            .Where(import => import.Module == GuestArrayCapabilityIntrinsics.Module)
            .ToArray();

        Assert(arrayImports.Select(import => import.Name).SequenceEqual(new[]
            {
                GuestArrayCapabilityIntrinsics.LengthImportName,
                GuestArrayCapabilityIntrinsics.ReadRangeImportName,
            }),
            "read-only array regions should not authorize or call the write-range import");
    }

    private static void UnreachableArrayOperationsDoNotInjectCapabilityImports()
    {
        const string source = """
            using System.Runtime.InteropServices;

            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "guest_value")]
                public static int Main() => 7;

                private static int Unreachable()
                {
                    int[] values = new[] { 1, 2 };
                    return values.Length;
                }
            }
            """;
        GuestModule module = Lower(source);

        Assert(module.Functions
                .SelectMany(function => function.Blocks)
                .SelectMany(block => block.Instructions)
                .All(instruction => instruction.Op is not
                    ("array_length" or "array_load" or "array_store"
                        or "array_region_load" or "array_region_store")),
            "unreachable array operations should stay outside the lowered function closure");
        Assert(module.Imports.All(import =>
                import.Id is not
                    (GuestArrayCapabilityIntrinsics.LengthImportId
                        or GuestArrayCapabilityIntrinsics.LoadImportId
                        or GuestArrayCapabilityIntrinsics.StoreImportId
                        or GuestArrayCapabilityIntrinsics.ReadRangeImportId
                        or GuestArrayCapabilityIntrinsics.WriteRangeImportId)),
            "unreachable array operations should not inject capability imports");
    }

    private static GuestModule Lower(string source)
    {
        const string sourceId = "Scripts/ArrayCapability.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        SemanticDocument semantic = SemanticAnalyzer.Analyze(
            source,
            sourceId,
            frontend.Source.Sha256,
            Array.Empty<SemanticReferenceSource>());
        CSharpGuestLoweringResult lowered = CSharpGuestLowerer.Lower(
            semantic,
            new string('c', 64));
        if (!frontend.Succeeded || !semantic.Succeeded || !lowered.Succeeded)
        {
            throw new InvalidOperationException(string.Join(
                " | ",
                frontend.Diagnostics.Select(item => $"{item.Code}:{item.Message}")
                    .Concat(semantic.Diagnostics.Select(item => $"{item.Code}:{item.Message}"))
                    .Concat(lowered.Diagnostics.Select(item => $"{item.Code}:{item.Message}"))));
        }

        return lowered.Module!;
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
