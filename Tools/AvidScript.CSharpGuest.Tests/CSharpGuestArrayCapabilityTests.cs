using System;
using System.IO;
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
        SynchronousArrayForeachUsesIndexedReads();
        ArrayForeachPreservesBreakContinueAndSingleEvaluation();
        NestedArrayForeachPreservesEarlyReturn();
        ExecutableReferenceSourceArrayForeachLowers();
        ArrayForeachRequiresCurrentSemanticContract();
        EnumeratorForeachRemainsRejected();
        CapturedIterationVariableRemainsRejected();
        ArrayForeachDoesNotBypassExplicitFinally();
        return 11;
    }

    private static void SynchronousArrayForeachUsesIndexedReads()
    {
        const string source = """
            using System.Runtime.InteropServices;

            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "array_foreach")]
                public static int Main() => Compute();
                public static int Compute()
                {
                    int[] values = new[] { 3, 5, 8 };
                    int sum = 0;
                    foreach (int value in values)
                    {
                        sum += value;
                    }
                    return sum;
                }
            }
            """;
        Assert(CSharpGuestBorrowedReferenceTests.Reference(source, methodName: "Compute") == 16,
            "the same ordinary C# source should return 16 under .NET");
        GuestModule module = Lower(source);
        GuestInstruction[] instructions = module.Functions
            .SelectMany(function => function.Blocks)
            .SelectMany(block => block.Instructions)
            .ToArray();
        Assert(instructions.Count(instruction => instruction.Op == "array_length") == 1
            && instructions.Count(instruction => instruction.Op == "array_region_load") == 1
            && instructions.All(instruction => !string.Equals(
                instruction.Op, "call_enumerator", StringComparison.Ordinal)),
            "synchronous array foreach should read each element through the indexed array capability path");
        Assert(module.Imports.Where(import => import.Module == GuestArrayCapabilityIntrinsics.Module)
            .Select(import => import.Name)
            .SequenceEqual(new[]
            {
                GuestArrayCapabilityIntrinsics.LengthImportName,
                GuestArrayCapabilityIntrinsics.ReadRangeImportName,
            }),
            "array foreach should authorize length and read capabilities without write access");
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(module);
        Assert(wasm.Succeeded && wasm.Bytes.Length > 8,
            "synchronous array foreach must compile to a nonempty WASM module");
        string? outputDirectory = Environment.GetEnvironmentVariable("AVIDSCRIPT_ARRAY_FOREACH_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(outputDirectory))
        {
            Directory.CreateDirectory(outputDirectory);
            File.WriteAllBytes(Path.Combine(outputDirectory, "array-foreach.wasm"), wasm.Bytes);
        }
    }

    private static void EnumeratorForeachRemainsRejected()
    {
        const string source = """
            using System.Collections.Generic;
            using System.Runtime.InteropServices;
            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "enumerator_foreach")]
                public static int Main()
                {
                    List<int> values = new() { 3, 5, 8 };
                    int sum = 0;
                    foreach (int value in values) sum += value;
                    return sum;
                }
            }
            """;
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, "Scripts/EnumeratorForeach.cs");
        SemanticDocument semantic = SemanticAnalyzer.Analyze(
            source, "Scripts/EnumeratorForeach.cs", frontend.Source.Sha256);
        Assert(frontend.Succeeded && !semantic.Succeeded
            && semantic.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS3001"),
            "enumerator foreach must retain the cleanup diagnostic until Dispose is modeled");
    }

    private static void ArrayForeachPreservesBreakContinueAndSingleEvaluation()
    {
        const string source = """
            using System.Runtime.InteropServices;
            public static class Script
            {
                private static int Calls;
                private static int[] Values()
                {
                    Calls++;
                    return new[] { 2, 3, 5, 7 };
                }
                [UnmanagedCallersOnly(EntryPoint = "array_foreach_branches")]
                public static int Main() => Compute();
                public static int Compute()
                {
                    int sum = 0;
                    foreach (int value in Values())
                    {
                        if (value == 3) continue;
                        if (value == 7) break;
                        sum += value;
                    }
                    return Calls * 100 + sum;
                }
            }
            """;
        Assert(CSharpGuestBorrowedReferenceTests.Reference(source, methodName: "Compute") == 107,
            "the same ordinary C# source should evaluate the collection once under .NET");
        GuestModule module = Lower(source);
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(module);
        Assert(wasm.Succeeded,
            "array foreach with branch transfers and a side-effectful collection must compile to WASM");
        string? outputDirectory = Environment.GetEnvironmentVariable("AVIDSCRIPT_ARRAY_FOREACH_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(outputDirectory))
        {
            Directory.CreateDirectory(outputDirectory);
            File.WriteAllBytes(Path.Combine(outputDirectory, "array-foreach-branches.wasm"), wasm.Bytes);
        }
    }

    private static void CapturedIterationVariableRemainsRejected()
    {
        const string source = """
            using System;
            using System.Runtime.InteropServices;
            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "captured_array_foreach")]
                public static int Main()
                {
                    int[] values = new[] { 3, 5, 8 };
                    Func<int> last = () => 0;
                    foreach (int value in values) last = () => value;
                    return last();
                }
            }
            """;
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, "Scripts/CapturedArrayForeach.cs");
        SemanticDocument semantic = SemanticAnalyzer.Analyze(
            source, "Scripts/CapturedArrayForeach.cs", frontend.Source.Sha256);
        Assert(frontend.Succeeded && !semantic.Succeeded
            && semantic.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS3001"),
            "iteration-variable captures must wait for a per-iteration closure environment");
    }

    private static void NestedArrayForeachPreservesEarlyReturn()
    {
        const string source = """
            using System.Runtime.InteropServices;
            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "array_foreach_nested_return")]
                public static int Main() => Compute();
                public static int Compute()
                {
                    int total = 0;
                    foreach (int x in new[] { 1, 2, 3 })
                    {
                        foreach (int y in new[] { 4, 5 })
                        {
                            if (x == 2 && y == 5) return total + 100;
                            total += x * y;
                        }
                    }
                    return total;
                }
            }
            """;
        Assert(CSharpGuestBorrowedReferenceTests.Reference(source, methodName: "Compute") == 117,
            "the nested early return should match ordinary .NET execution");
        GuestModule module = Lower(source);
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(module);
        Assert(wasm.Succeeded, "nested array foreach with an early value return must compile to WASM");
        string? outputDirectory = Environment.GetEnvironmentVariable("AVIDSCRIPT_ARRAY_FOREACH_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(outputDirectory))
        {
            Directory.CreateDirectory(outputDirectory);
            File.WriteAllBytes(Path.Combine(outputDirectory, "array-foreach-nested-return.wasm"), wasm.Bytes);
        }
    }

    private static void ArrayForeachDoesNotBypassExplicitFinally()
    {
        const string source = """
            using System.Runtime.InteropServices;
            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "array_foreach_finally")]
                public static int Main()
                {
                    int sum = 0;
                    try
                    {
                        foreach (int value in new[] { 3, 5, 8 }) sum += value;
                    }
                    finally
                    {
                        sum += 10;
                    }
                    return sum;
                }
            }
            """;
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, "Scripts/ArrayForeachFinally.cs");
        SemanticDocument semantic = SemanticAnalyzer.Analyze(
            source, "Scripts/ArrayForeachFinally.cs", frontend.Source.Sha256);
        Assert(frontend.Succeeded && !semantic.Succeeded
            && semantic.Diagnostics.Any(diagnostic => diagnostic.Code.StartsWith("ASCS", StringComparison.Ordinal)),
            "array foreach must not drop an explicit finally that the synchronous plan cannot execute");
    }

    private static void ExecutableReferenceSourceArrayForeachLowers()
    {
        const string source = """
            using System.Runtime.InteropServices;
            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "array_foreach_reference")]
                public static int Main() => GeneratedLibrary.Sum();
            }
            """;
        const string reference = """
            public static class GeneratedLibrary
            {
                public static int Sum()
                {
                    int[] values = new[] { 1, 2, 3 };
                    int sum = 0;
                    foreach (int value in values) sum += value;
                    return sum;
                }
            }
            """;
        const string sourceId = "Scripts/ArrayForeachReference.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        SemanticDocument semantic = SemanticAnalyzer.Analyze(
            source,
            sourceId,
            frontend.Source.Sha256,
            new[] { new SemanticReferenceSource(reference, "generated://ArrayForeachLibrary.cs", true) });
        CSharpGuestLoweringResult lowered = CSharpGuestLowerer.Lower(semantic, new string('c', 64));
        Assert(semantic.Succeeded && lowered.Succeeded,
            "executable reference source array foreach should keep its own symbols and source spans: "
                + string.Join(" | ", semantic.Diagnostics.Select(diagnostic => diagnostic.Message)
                    .Concat(lowered.Diagnostics.Select(diagnostic => diagnostic.Message))));
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(lowered.Module!);
        Assert(wasm.Succeeded, "reference source array foreach must compile to WASM");
        string? outputDirectory = Environment.GetEnvironmentVariable("AVIDSCRIPT_ARRAY_FOREACH_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(outputDirectory))
        {
            Directory.CreateDirectory(outputDirectory);
            File.WriteAllBytes(Path.Combine(outputDirectory, "array-foreach-reference.wasm"), wasm.Bytes);
        }
    }

    private static void ArrayForeachRequiresCurrentSemanticContract()
    {
        const string source = """
            using System.Runtime.InteropServices;
            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "array_foreach_version")]
                public static int Main()
                {
                    int sum = 0;
                    foreach (int value in new[] { 3, 5, 8 }) sum += value;
                    return sum;
                }
            }
            """;
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, "Scripts/ArrayForeachVersion.cs");
        SemanticDocument semantic = SemanticAnalyzer.Analyze(
            source, "Scripts/ArrayForeachVersion.cs", frontend.Source.Sha256);
        Assert(semantic.Succeeded && semantic.SemanticVersion == "1.38"
            && semantic.Symbols.Any(symbol => symbol.Id.StartsWith(
                "symbol:compiler_local:", StringComparison.Ordinal)),
            "array foreach should advertise the versioned synchronous iteration plan");
        CSharpGuestLoweringResult downgraded = CSharpGuestLowerer.Lower(
            semantic with { SchemaVersion = 30, SemanticVersion = "1.34" },
            new string('c', 64));
        Assert(!downgraded.Succeeded,
            "schema 30 / semantic 1.34 cannot claim the new array iteration plan");

        const string oldSource = """
            using System.Runtime.InteropServices;
            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "old_semantic")]
                public static int Main() => 7;
            }
            """;
        FrontendDocument oldFrontend = FrontendAnalyzer.Analyze(oldSource, "Scripts/OldSemantic.cs");
        SemanticDocument oldSemantic = SemanticAnalyzer.Analyze(
            oldSource, "Scripts/OldSemantic.cs", oldFrontend.Source.Sha256);
        Assert(CSharpGuestLowerer.Lower(
            oldSemantic with { SchemaVersion = 30, SemanticVersion = "1.34" },
            new string('c', 64)).Succeeded,
            "a prior semantic 1.34 artifact without new compiler locals should stay readable");
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
