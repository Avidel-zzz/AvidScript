using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestDebugResumableTests
{
    private const string SemanticHash =
        "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";

    public static int Run()
    {
        ExportAbiAndRuntimeSurfaceRemainStable();
        PauseSpillsAndRestoresBoundedAggregateFrame();
        RoutesAreUniqueDeterministicAndPreserveLoopCfg();
        NestedGuestTargetsAndOversizedFramesFailClosed();
        return 4;
    }

    private static void ExportAbiAndRuntimeSurfaceRemainStable()
    {
        const string source = """
            using System.Runtime.InteropServices;

            namespace Game;

            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "guest_main")]
                public static void Main(int value)
                {
                    Sink(value + 1);
                }

                [UnmanagedCallersOnly(EntryPoint = "guest_value")]
                public static int Value(int value)
                {
                    return value + 1;
                }

                [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
                public static void EndPlay()
                {
                    Sink(0);
                }

                private static void Sink(int value) { }
            }
            """;
        LoweringPair pair = Lower(source, "Scripts/DebugResumableAbi.cs");
        GuestExport baselineMain = pair.Baseline.Exports.Single(item => item.Name == "guest_main");
        GuestExport enabledMain = pair.Enabled.Exports.Single(item => item.Name == "guest_main");
        GuestExport baselineValue = pair.Baseline.Exports.Single(item => item.Name == "guest_value");
        GuestExport enabledValue = pair.Enabled.Exports.Single(item => item.Name == "guest_value");
        GuestExport baselineEndPlay = pair.Baseline.Exports.Single(item =>
            item.Name == "avid_on_end_play");
        GuestExport enabledEndPlay = pair.Enabled.Exports.Single(item =>
            item.Name == "avid_on_end_play");
        GuestFunction baselineMainFunction = pair.Baseline.Functions.Single(item =>
            item.Id == baselineMain.FunctionId);
        GuestFunction wrapper = pair.Enabled.Functions.Single(item =>
            item.Id == enabledMain.FunctionId);
        GuestFunction machine = pair.Enabled.Functions.Single(item =>
            item.Id == baselineMain.FunctionId);
        GuestFunction resume = pair.Enabled.Functions.Single(item =>
            item.Id == pair.Enabled.Exports.Single(export =>
                export.Name == "avid_on_debug_resume").FunctionId);

        Assert(enabledMain.FunctionId != baselineMain.FunctionId
            && wrapper.Parameters.Select(item => item.TypeId)
                .SequenceEqual(baselineMainFunction.Parameters.Select(item => item.TypeId))
            && wrapper.ReturnTypeId == baselineMainFunction.ReturnTypeId
            && machine.Parameters.Count == baselineMainFunction.Parameters.Count + 2,
            "the public void export should retain its original ABI through a fresh-call wrapper");
        Assert(enabledValue.FunctionId == baselineValue.FunctionId
            && pair.Enabled.Functions.Single(item => item.Id == enabledValue.FunctionId)
                .Blocks.SelectMany(block => block.Instructions)
                .All(instruction => instruction.TargetId != CSharpGuestDebugProbeAbi.ImportId),
            "non-void direct exports should remain untouched and uninstrumented");
        Assert(enabledEndPlay.FunctionId == baselineEndPlay.FunctionId
            && pair.Enabled.Functions.Single(item => item.Id == enabledEndPlay.FunctionId)
                .Blocks.SelectMany(block => block.Instructions)
                .All(instruction => instruction.TargetId != CSharpGuestDebugProbeAbi.ImportId),
            $"EndPlay should remain non-resumable until teardown can preserve its host epilogue: baseline={baselineEndPlay.FunctionId}, enabled={enabledEndPlay.FunctionId}");
        Assert(resume.Parameters.Select(item => item.TypeId).SequenceEqual(new[]
            {
                CSharpGuestDebugProbeAbi.ProbeIdTypeId,
                CSharpGuestDebugProbeAbi.ActionTypeId,
            })
            && resume.ReturnTypeId == "type:void",
            "avid_on_debug_resume should expose the frozen (i64 token, i32 route) -> void ABI");

        GuestImport probe = pair.Enabled.Imports.Single(item =>
            item.Id == CSharpGuestDebugProbeAbi.ImportId);
        GuestImport suspend = pair.Enabled.Imports.Single(item =>
            item.Id == CSharpGuestDebugProbeAbi.SuspendImportId);
        GuestImport frameRead = pair.Enabled.Imports.Single(item =>
            item.Id == CSharpGuestDebugProbeAbi.FrameReadImportId);
        Assert(probe.ParameterTypeIds.SequenceEqual(new[] { "type:int64" })
            && probe.ReturnTypeId == "type:int32"
            && suspend.ParameterTypeIds.SequenceEqual(new[]
            {
                "type:int64", "type:int32", "type:address", "type:int32",
            })
            && suspend.ReturnTypeId == "type:int64"
            && frameRead.ParameterTypeIds.SequenceEqual(new[]
            {
                "type:int64", "type:address", "type:int32",
            })
            && frameRead.ReturnTypeId == "type:int32"
            && new[] { probe, suspend, frameRead }.All(item =>
                item.Module == CSharpGuestDebugProbeAbi.ModuleName
                && item.DispatchClass == "debug"),
            "debug-enabled modules should publish the three backend-neutral debug imports");
        Assert(GuestModuleValidator.Validate(pair.Enabled).Succeeded
            && WasmModuleCompiler.Compile(pair.Enabled).Succeeded,
            "the wrapper, machine, and resume router should validate and compile through WasmBackend");
    }

    private static void PauseSpillsAndRestoresBoundedAggregateFrame()
    {
        const string source = """
            using System.Runtime.InteropServices;

            namespace Game;

            public struct Pair
            {
                public int X;
                public int Y;
            }

            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "guest_main")]
                public static void Main(Pair input)
                {
                    Pair local = input;
                    Consume(local);
                }

                private static void Consume(Pair value) { }
            }
            """;
        LoweringPair pair = Lower(source, "Scripts/DebugResumableFrame.cs");
        string machineId = pair.Baseline.Exports.Single(item => item.Name == "guest_main").FunctionId;
        GuestFunction baseline = pair.Baseline.Functions.Single(item => item.Id == machineId);
        GuestFunction machine = pair.Enabled.Functions.Single(item => item.Id == machineId);
        GuestFunction resume = pair.Enabled.Functions.Single(item =>
            item.Id == pair.Enabled.Exports.Single(export =>
                export.Name == "avid_on_debug_resume").FunctionId);
        GuestType frame = pair.Enabled.Types.Single(item =>
            item.Id.StartsWith("type:synthetic:debug_resumable_frame:v1:", StringComparison.Ordinal));
        GuestType pairType = pair.Enabled.Types.Single(item =>
            item.Id == baseline.Parameters.Single().TypeId);
        Dictionary<string, GuestInstruction> definitions = machine.Blocks
            .SelectMany(block => block.Instructions)
            .Where(instruction => instruction.ResultId is not null)
            .ToDictionary(instruction => instruction.ResultId!, StringComparer.Ordinal);
        GuestBasicBlock[] pauseBlocks = machine.Blocks.Where(block =>
            block.Instructions.Any(instruction => instruction.Op == "call"
                && instruction.TargetId == CSharpGuestDebugProbeAbi.SuspendImportId)).ToArray();

        Assert(frame.Size > 0
            && frame.Size <= 4096
            && frame.Fields.Count == 1 + baseline.Parameters.Count + baseline.Locals.Count,
            "the bounded frame should contain a route header plus every original parameter and local");
        Assert(pauseBlocks.Length > 0 && pauseBlocks.All(block =>
            block.Instructions.Count(instruction => instruction.Op == "field_store")
                == frame.Fields.Count),
            "every pause path should spill the complete frame before suspension");
        foreach (GuestBasicBlock pauseBlock in pauseBlocks)
        {
            GuestInstruction suspend = pauseBlock.Instructions.Single(instruction =>
                instruction.Op == "call"
                && instruction.TargetId == CSharpGuestDebugProbeAbi.SuspendImportId);
            int route = Int32Constant(definitions[suspend.OperandIds[1]]);
            int frameBytes = Int32Constant(definitions[suspend.OperandIds[3]]);
            GuestInstruction accepted = definitions[pauseBlock.Terminator.ConditionValueId!];
            Assert(route > 0
                && frameBytes == frame.Size
                && accepted.Op == "binary"
                && accepted.OperatorKind == "greater_than",
                "suspend should receive a positive route and exact frame bounds, then require token > 0");
        }
        Assert(frame.Fields.Any(field => field.TypeId == pairType.Id)
            && machine.Blocks.SelectMany(block => block.Instructions).Any(instruction =>
                instruction.Op == "field_store"
                && instruction.TargetId is not null
                && frame.Fields.Any(field => field.Id == instruction.TargetId
                    && field.TypeId == pairType.Id))
            && machine.Blocks.SelectMany(block => block.Instructions).Any(instruction =>
                instruction.Op == "field_load"
                && instruction.TargetId is not null
                && frame.Fields.Any(field => field.Id == instruction.TargetId
                    && field.TypeId == pairType.Id)),
            "aggregate parameters and locals should deep-copy through frame field stores and loads");
        Assert(machine.Blocks.SelectMany(block => block.Instructions).Count(instruction =>
                instruction.Op == "call"
                && instruction.TargetId == CSharpGuestDebugProbeAbi.FrameReadImportId) == 1
            && resume.Blocks.SelectMany(block => block.Instructions).Any(instruction =>
                instruction.Op == "stack_alloc"
                && instruction.ResultId is not null
                && resume.Locals.Single(local => local.Id == instruction.ResultId).TypeId
                    == pairType.Id)
            && GuestModuleValidator.Validate(pair.Enabled).Succeeded
            && WasmModuleCompiler.Compile(pair.Enabled).Succeeded,
            "resume should read one frame, restore it, and remain consumable by WasmBackend");
    }

    private static void RoutesAreUniqueDeterministicAndPreserveLoopCfg()
    {
        const string source = """
            using System.Runtime.InteropServices;

            namespace Game;

            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "guest_alpha")]
                public static void Alpha(int count)
                {
                    int total = 0;
                    while (count > 0)
                    {
                        total = Helper(total);
                        count = count - 1;
                    }
                }

                [UnmanagedCallersOnly(EntryPoint = "guest_beta")]
                public static void Beta()
                {
                    Helper(1);
                }

                private static int Helper(int value)
                {
                    return value + 1;
                }
            }
            """;
        LoweringPair first = Lower(source, "Scripts/DebugResumableLoop.cs");
        LoweringPair second = Lower(source, "Scripts/DebugResumableLoop.cs");
        int[] firstRoutes = SuspendRoutes(first.Enabled);
        int[] secondRoutes = SuspendRoutes(second.Enabled);
        Assert(firstRoutes.Length > 1
            && firstRoutes.All(route => route > 0)
            && firstRoutes.Distinct().Count() == firstRoutes.Length
            && firstRoutes.SequenceEqual(secondRoutes),
            "resume routes should be positive, module-unique, and deterministic for equivalent input");

        string alphaId = first.Baseline.Exports.Single(item => item.Name == "guest_alpha").FunctionId;
        GuestFunction baselineAlpha = first.Baseline.Functions.Single(item => item.Id == alphaId);
        GuestFunction machineAlpha = first.Enabled.Functions.Single(item => item.Id == alphaId);
        HashSet<string> baselineEdges = Edges(baselineAlpha);
        HashSet<string> machineEdges = Edges(machineAlpha);
        Dictionary<string, int> baselineOrdinals = baselineAlpha.Blocks
            .Select((block, index) => (block.Id, index))
            .ToDictionary(item => item.Id, item => item.index, StringComparer.Ordinal);
        bool hasBackEdge = baselineAlpha.Blocks.Select((block, index) => (block, index)).Any(item =>
            Targets(item.block.Terminator).Any(target =>
                baselineOrdinals.TryGetValue(target, out int targetIndex)
                && targetIndex <= item.index));
        Assert(hasBackEdge && baselineEdges.IsSubsetOf(machineEdges),
            "instrumentation should retain the original loop and branch CFG edges behind continue blocks");

        string helperMethodId = first.Semantic.Callables.Single(callable =>
            callable.MethodSymbolId.Contains(".Helper(", StringComparison.Ordinal)).MethodSymbolId;
        GuestFunction helper = first.Enabled.Functions.Single(item =>
            item.Id == "function:" + helperMethodId);
        Assert(helper.Blocks.SelectMany(block => block.Instructions).All(instruction =>
                instruction.TargetId != CSharpGuestDebugProbeAbi.ImportId
                && instruction.TargetId != CSharpGuestDebugProbeAbi.SuspendImportId
                && instruction.TargetId != CSharpGuestDebugProbeAbi.FrameReadImportId),
            "ordinary helpers reached from an export should not receive pausable probes");
        Assert(GuestIrSerializer.Serialize(first.Enabled)
                .SequenceEqual(GuestIrSerializer.Serialize(second.Enabled))
            && GuestModuleValidator.Validate(first.Enabled).Succeeded
            && WasmModuleCompiler.Compile(first.Enabled).Succeeded,
            "equivalent input should emit byte-identical valid Guest IR that compiles through WasmBackend");
    }

    private static void NestedGuestTargetsAndOversizedFramesFailClosed()
    {
        const string source = """
            using System.Runtime.InteropServices;

            namespace Game;

            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "guest_outer")]
                public static void Outer()
                {
                    Inner();
                }

                public static void Inner()
                {
                    int value = 1;
                    value = value + 1;
                }
            }
            """;
        LoweringPair pair = Lower(source, "Scripts/DebugResumableNested.cs");
        string innerMethodId = pair.Semantic.Callables.Single(callable =>
            callable.MethodSymbolId.Contains(".Inner(", StringComparison.Ordinal)).MethodSymbolId;
        string innerFunctionId = "function:" + innerMethodId;
        Assert(pair.Enabled.Functions.Single(item => item.Id == innerFunctionId)
                .Blocks.SelectMany(block => block.Instructions)
                .All(instruction => instruction.TargetId != CSharpGuestDebugProbeAbi.ImportId),
            "a Guest helper call target should stay non-resumable so a nested pause cannot unwind its caller");

        string declarations = string.Join(
            Environment.NewLine,
            Enumerable.Range(0, 1025).Select(index => $"int value{index} = {index};"));
        string oversizedSource = $$"""
            using System.Runtime.InteropServices;

            namespace Game;

            public static class OversizedScript
            {
                [UnmanagedCallersOnly(EntryPoint = "guest_oversized")]
                public static void Main()
                {
                    {{declarations}}
                }
            }
            """;
        FrontendDocument oversizedFrontend = FrontendAnalyzer.Analyze(
            oversizedSource,
            "Scripts/DebugResumableOversized.cs");
        SemanticDocument oversizedSemantic = SemanticAnalyzer.Analyze(
            oversizedSource,
            oversizedFrontend.Source.SourceId,
            oversizedFrontend.Source.Sha256);
        bool rejected = false;
        try
        {
            CSharpGuestLowerer.Lower(
                oversizedSemantic,
                SemanticHash,
                enableDebugInstrumentation: true);
        }
        catch (InvalidOperationException exception)
        {
            rejected = exception.Message.StartsWith("ASDEBUG1102:", StringComparison.Ordinal);
        }
        Assert(rejected, "debug frames larger than 4 KiB should fail closed before WASM codegen");
    }

    private static LoweringPair Lower(string source, string sourceId)
    {
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        SemanticDocument semantic = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);
        Assert(semantic.Succeeded,
            "debug resumable fixture should analyze: "
                + string.Join(" | ", semantic.Diagnostics.Select(item => item.Message)));
        CSharpGuestLoweringResult baselineResult = CSharpGuestLowerer.Lower(
            semantic,
            SemanticHash);
        CSharpGuestLoweringResult enabledResult = CSharpGuestLowerer.Lower(
            semantic,
            SemanticHash,
            enableDebugInstrumentation: true);
        GuestModule baseline = baselineResult.Module
            ?? throw new InvalidOperationException(
                "baseline debug fixture should lower: "
                    + string.Join(" | ", baselineResult.Diagnostics.Select(item => item.Message)));
        GuestModule enabled = enabledResult.Module
            ?? throw new InvalidOperationException(
                "enabled debug fixture should lower: "
                    + string.Join(" | ", enabledResult.Diagnostics.Select(item => item.Message)));
        return new LoweringPair(semantic, baseline, enabled);
    }

    private static int[] SuspendRoutes(GuestModule module)
    {
        List<int> routes = new();
        foreach (GuestFunction function in module.Functions)
        {
            Dictionary<string, GuestInstruction> definitions = function.Blocks
                .SelectMany(block => block.Instructions)
                .Where(instruction => instruction.ResultId is not null)
                .ToDictionary(instruction => instruction.ResultId!, StringComparer.Ordinal);
            routes.AddRange(function.Blocks
                .SelectMany(block => block.Instructions)
                .Where(instruction => instruction.Op == "call"
                    && instruction.TargetId == CSharpGuestDebugProbeAbi.SuspendImportId)
                .Select(instruction => Int32Constant(definitions[instruction.OperandIds[1]])));
        }
        return routes.ToArray();
    }

    private static int Int32Constant(GuestInstruction instruction)
    {
        return int.Parse(
            instruction.Constant?.Value
                ?? throw new InvalidOperationException("expected an i32 constant"),
            CultureInfo.InvariantCulture);
    }

    private static HashSet<string> Edges(GuestFunction function)
    {
        return function.Blocks
            .SelectMany(block => Targets(block.Terminator).Select(target =>
                block.Terminator.Kind + "|" + target))
            .ToHashSet(StringComparer.Ordinal);
    }

    private static IEnumerable<string> Targets(GuestTerminator terminator)
    {
        if (terminator.TargetBlockId is not null)
        {
            yield return terminator.TargetBlockId;
        }
        if (terminator.FalseTargetBlockId is not null)
        {
            yield return terminator.FalseTargetBlockId;
        }
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }

    private sealed record LoweringPair(
        SemanticDocument Semantic,
        GuestModule Baseline,
        GuestModule Enabled);
}
