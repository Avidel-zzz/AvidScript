using System;
using System.Collections.Generic;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpDataLaneFusionTests
{
    private const string SemanticHash = "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";

    public static int Run()
    {
        ThreeSettersProduceExecutableCommandBuffer();
        UserSetterBodiesNeverFuse();
        TwoSettersDoNotFuse();
        CallsStoresAndReadAfterWriteSplitFusionGroups();
        PotentiallyTrappingPreparationSplitsFusionGroups();
        DirectExternPropertyPreservesImportMetadata();
        FusionCanBeDisabledPerProfile();
        HostAbiBudgetSplitsLargeGroups();
        return 8;
    }

    private static void ThreeSettersProduceExecutableCommandBuffer()
    {
        const string body = """
            AvidScript.Generated.Set(target, 10);
            AvidScript.Generated.Set(target, 20);
            AvidScript.Generated.Set(target, 30);
            """;
        Lowered first = Lower(body, "Scripts/DataLaneThreeSetters.cs");
        Lowered second = Lower(body, "Scripts/DataLaneThreeSetters.cs");
        GuestModule module = first.Module;
        GuestFunction function = MainFunction(module);
        GuestInstruction[] instructions = function.Blocks
            .SelectMany(block => block.Instructions)
            .ToArray();

        GuestImport epoch = module.Imports.Single(import =>
            import.Module == "avidscript"
            && import.Name == "avid_data_lane_epoch"
            && import.DispatchClass == "data_lane");
        GuestImport submit = module.Imports.Single(import =>
            import.Module == "avidscript"
            && import.Name == "avid_data_lane_submit"
            && import.DispatchClass == "data_lane");
        Assert(epoch.ParameterTypeIds.Count == 0 && epoch.ReturnTypeId == "type:uint64",
            "data-lane epoch should use the fixed ()I signature");
        Assert(submit.ParameterTypeIds.SequenceEqual(new[] { "type:address", "type:int32" })
            && submit.ReturnTypeId == "type:int32",
            "data-lane submit should use the fixed (ii)i signature");

        GuestImport setterImport = module.Imports.Single(import =>
            import.Name == "avid_generated_property_set");
        Assert(setterImport.OptimizationClass == "buffered_write"
            && setterImport.BindingOrdinal == 17,
            "the generated extern setter import should retain verified fusion metadata");

        SemanticCallable setter = first.Semantic.Callables.Single(callable =>
            callable.Import?.Name == "avid_generated_property_set");
        Assert(!setter.HasBody && setter.Import is not null,
            "fusion should target only the executable generated extern setter");
        string setterTargetId = "import:" + setter.MethodSymbolId;
        Assert(!instructions.Any(instruction =>
                instruction.Op == "call" && instruction.TargetId == setterTargetId),
            "the three original setter calls should be removed from the caller");
        Assert(instructions.Count(instruction =>
                instruction.Op == "call" && instruction.TargetId == epoch.Id) == 1,
            "one fused group should call epoch exactly once");
        GuestInstruction submitCall = instructions.Single(instruction =>
            instruction.Op == "call" && instruction.TargetId == submit.Id);

        GuestType bufferType = module.Types.Single(type =>
            type.Id.StartsWith(
                "type:__avidscript_internal.data_lane_command_buffer.3",
                StringComparison.Ordinal));
        Assert(bufferType.Kind == "struct" && bufferType.Size == 120 && bufferType.Alignment == 8,
            "three commands should occupy a 24-byte header plus three 32-byte records");
        Assert(Field(bufferType, "Header.Magic").Offset == 0
            && Field(bufferType, "Header.Schema").Offset == 4
            && Field(bufferType, "Header.Count").Offset == 6
            && Field(bufferType, "Header.ByteCount").Offset == 8
            && Field(bufferType, "Header.Reserved").Offset == 12
            && Field(bufferType, "Header.CallbackEpoch").Offset == 16,
            "command-buffer header fields should use the fixed little-endian ABI offsets");
        for (int index = 0; index < 3; ++index)
        {
            int commandOffset = 24 + (32 * index);
            Assert(Field(bufferType, $"Command[{index}].Opcode").Offset == commandOffset
                && Field(bufferType, $"Command[{index}].Flags").Offset == commandOffset + 2
                && Field(bufferType, $"Command[{index}].RecordBytes").Offset == commandOffset + 4
                && Field(bufferType, $"Command[{index}].BindingOrdinal").Offset == commandOffset + 8
                && Field(bufferType, $"Command[{index}].SelfSlot").Offset == commandOffset + 12
                && Field(bufferType, $"Command[{index}].SelfGeneration").Offset == commandOffset + 16
                && Field(bufferType, $"Command[{index}].Value").Offset == commandOffset + 20
                && Field(bufferType, $"Command[{index}].Arg1").Offset == commandOffset + 24
                && Field(bufferType, $"Command[{index}].Reserved").Offset == commandOffset + 28,
                $"command {index} should use the fixed 32-byte SetI32 layout");
        }

        IReadOnlyDictionary<string, GuestInstruction> definitions = instructions
            .Where(instruction => instruction.ResultId is not null)
            .ToDictionary(instruction => instruction.ResultId!, StringComparer.Ordinal);
        Assert(StoredUInt32(bufferType, instructions, definitions, "Header.Magic") == 0x41564342u
            && StoredUInt32(bufferType, instructions, definitions, "Header.Schema") == 1u
            && StoredUInt32(bufferType, instructions, definitions, "Header.Count") == 3u
            && StoredUInt32(bufferType, instructions, definitions, "Header.ByteCount") == 120u
            && StoredUInt32(bufferType, instructions, definitions, "Header.Reserved") == 0u,
            "the fused header should contain the fixed magic, schema, count, byte count, and reserved value");
        GuestInstruction epochStore = StoreTo(bufferType, instructions, "Header.CallbackEpoch");
        Assert(definitions[epochStore.OperandIds[1]].TargetId == epoch.Id,
            "the callback epoch field should receive the real epoch import result");

        for (int index = 0; index < 3; ++index)
        {
            Assert(StoredUInt32(bufferType, instructions, definitions, $"Command[{index}].Opcode") == 1u
                && StoredUInt32(bufferType, instructions, definitions, $"Command[{index}].Flags") == 0u
                && StoredUInt32(bufferType, instructions, definitions, $"Command[{index}].RecordBytes") == 32u
                && StoredUInt32(bufferType, instructions, definitions, $"Command[{index}].BindingOrdinal") == 17u
                && StoredUInt32(bufferType, instructions, definitions, $"Command[{index}].Arg1") == 0u
                && StoredUInt32(bufferType, instructions, definitions, $"Command[{index}].Reserved") == 0u,
                $"command {index} should encode the fixed SetI32 metadata");
            Assert(StoredInt32(
                    bufferType,
                    instructions,
                    definitions,
                    $"Command[{index}].Value") == 10 * (index + 1),
                $"command {index} should preserve its original setter value");
            GuestInstruction slot = definitions[
                StoreTo(bufferType, instructions, $"Command[{index}].SelfSlot").OperandIds[1]];
            GuestInstruction generation = definitions[
                StoreTo(bufferType, instructions, $"Command[{index}].SelfGeneration").OperandIds[1]];
            Assert(slot.Op == "field_load" && slot.TargetId?.EndsWith(".Slot:int32", StringComparison.Ordinal) == true
                && generation.Op == "field_load"
                && generation.TargetId?.EndsWith(".Generation:int32", StringComparison.Ordinal) == true,
                $"command {index} should carry the receiver's Slot and Generation fields");
        }

        Assert(submitCall.OperandIds.Count == 2
            && definitions[submitCall.OperandIds[0]].Op == "address_of"
            && definitions[submitCall.OperandIds[1]].Constant?.Value == "120",
            "submit should receive the command-buffer address and checked byte count");
        string submitStatusId = submitCall.ResultId
            ?? throw new InvalidOperationException("submit should define an internal status local");
        Assert(!instructions.Any(instruction => instruction.OperandIds.Contains(submitStatusId))
            && function.Blocks.All(block => block.Terminator.ReturnValueId != submitStatusId
                && block.Terminator.ConditionValueId != submitStatusId),
            "the internal submit status local should not be reused by user IR");

        string firstChecksum = Checksum(first.Module);
        string secondChecksum = Checksum(second.Module);
        Assert(firstChecksum == secondChecksum,
            "identical semantic input should produce the same fused GuestIR checksum");

        WasmCompilationResult wasm = WasmModuleCompiler.Compile(module);
        Assert(wasm.Succeeded
            && wasm.Bytes.Length > 8
            && wasm.Bytes[0] == 0x00
            && wasm.Bytes[1] == 0x61
            && wasm.Bytes[2] == 0x73
            && wasm.Bytes[3] == 0x6d,
            "fused stack allocation and 16-bit field stores should compile to a WASM module");
    }

    private static void TwoSettersDoNotFuse()
    {
        const string body = """
            AvidScript.Generated.Set(target, 1);
            AvidScript.Generated.Set(target, 2);
            """;
        GuestModule module = Lower(body, "Scripts/DataLaneTwoSetters.cs").Module;

        Assert(!module.Imports.Any(import => import.DispatchClass == "data_lane"),
            "two setter calls should not add data-lane imports");
        Assert(MainFunction(module).Blocks
                .SelectMany(block => block.Instructions)
                .Count(instruction => instruction.Op == "call"
                    && instruction.TargetId?.Contains(".Set(", StringComparison.Ordinal) == true) == 2,
            "two setter calls should remain as ordinary executable calls");
    }

    private static void UserSetterBodiesNeverFuse()
    {
        const string source = """
            using System;
            using System.Runtime.InteropServices;

            namespace AvidScript
            {
                [AttributeUsage(AttributeTargets.Method)]
                public sealed class AvidScriptDataLaneAttribute : Attribute
                {
                    public AvidScriptDataLaneAttribute(string optimizationClass, int bindingOrdinal) { }
                }

                public struct Target
                {
                    public readonly int Slot;
                    public readonly int Generation;

                    public int Value
                    {
                        [AvidScriptDataLane("buffered_write", 17)]
                        set { _ = Native.Set(Slot, Generation, value); }
                    }
                }

                internal static class Native
                {
                    [DllImport("avidscript", EntryPoint = "avid_property_set_i32")]
                    internal static extern int Set(int slot, int generation, int value);
                }
            }

            namespace Game
            {
                public static class Script
                {
                    [UnmanagedCallersOnly(EntryPoint = "avid_data_lane_user_body")]
                    public static void Main(AvidScript.Target target)
                    {
                        target.Value = 1;
                        target.Value = 2;
                        target.Value = 3;
                    }
                }
            }
            """;
        Lowered lowered = LowerSource(source, "Scripts/DataLaneUserBody.cs");
        GuestInstruction[] instructions = MainFunction(lowered.Module).Blocks
            .SelectMany(block => block.Instructions)
            .ToArray();

        SemanticCallable setter = lowered.Semantic.Callables.Single(callable =>
            callable.AssociatedSymbolId is not null
            && callable.MethodSymbolId.Contains(".set_Value(", StringComparison.Ordinal));
        Assert(setter.HasBody && setter.Optimization is null,
            "user-authored setter bodies must not receive buffered-write fusion metadata");
        Assert(!lowered.Module.Imports.Any(import => import.DispatchClass == "data_lane")
            && instructions.Count(instruction =>
                instruction.Op == "call"
                && instruction.TargetId == "function:" + setter.MethodSymbolId) == 3,
            "user-authored setter bodies must remain ordinary calls");
    }

    private static void CallsStoresAndReadAfterWriteSplitFusionGroups()
    {
        const string body = """
            AvidScript.Generated.Set(target, 1);
            AvidScript.Generated.Set(target, 2);
            AvidScript.Native.Probe();
            AvidScript.Generated.Set(target, 3);
            AvidScript.Generated.Set(target, 4);
            int observed = target.Slot;
            AvidScript.Generated.Set(target, 5);
            AvidScript.Generated.Set(target, 6);
            observed = 7;
            AvidScript.Generated.Set(target, observed);
            AvidScript.Generated.Set(target, 8);
            """;
        GuestModule module = Lower(body, "Scripts/DataLaneBarriers.cs").Module;

        Assert(!module.Imports.Any(import => import.DispatchClass == "data_lane"),
            "unknown calls, getter reads, and stores should split sub-threshold setter groups");
        Assert(MainFunction(module).Blocks
                .SelectMany(block => block.Instructions)
                .Count(instruction => instruction.Op == "call"
                    && instruction.TargetId?.Contains(".Set(", StringComparison.Ordinal) == true) == 8,
            "barriers should retain every original setter call");
    }

    private static void PotentiallyTrappingPreparationSplitsFusionGroups()
    {
        const string body = """
            AvidScript.Generated.Set(target, 1);
            AvidScript.Generated.Set(target, 2);
            int computed = 100 / target.Slot;
            AvidScript.Generated.Set(target, computed);
            AvidScript.Generated.Set(target, 4);
            AvidScript.Generated.Set(target, 5);
            """;
        GuestModule module = Lower(body, "Scripts/DataLaneTrappingPreparation.cs").Module;
        GuestFunction function = MainFunction(module);
        GuestInstruction[] instructions = function.Blocks
            .SelectMany(block => block.Instructions)
            .ToArray();

        Assert(module.Imports.Count(import => import.DispatchClass == "data_lane") == 2,
            "the three setters after a potentially trapping binary operation should still fuse");
        Assert(instructions.Count(instruction =>
                instruction.Op == "call"
                && instruction.TargetId?.Contains(".Set(", StringComparison.Ordinal) == true) == 2,
            "writes before a potentially trapping binary operation must remain observable before the trap");
        Assert(instructions.Count(instruction =>
                instruction.Op == "binary") == 1,
            "the original potentially trapping binary operation should remain in place");
    }

    private static void DirectExternPropertyPreservesImportMetadata()
    {
        const string source = """
            using System.Runtime.InteropServices;

            namespace AvidScript
            {
                public static class DirectImport
                {
                    public static extern int Value
                    {
                        [AvidScriptDataLane("buffered_write", 29)]
                        [DllImport("avidscript", EntryPoint = "avid_direct_property_set")]
                        set;
                    }
                }
            }
            """;
        GuestModule module = LowerSource(
            "namespace Game { public static class Script { public static void Main() { } } }",
            "Scripts/DirectExternDataLaneProperty.cs",
            new[]
            {
                new SemanticReferenceSource(
                    DataLaneAttributeReferenceSource,
                    "generated://AvidScriptDataLaneAttribute.cs",
                    IsExecutable: false),
                new SemanticReferenceSource(
                    source,
                    "generated://AvidScript.DirectExtern.generated.cs",
                    IsExecutable: true),
            }).Module;
        GuestImport propertyImport = module.Imports.Single(import =>
            import.Name == "avid_direct_property_set");

        Assert(propertyImport.OptimizationClass == "buffered_write"
            && propertyImport.BindingOrdinal == 29,
            "LowerImports should preserve metadata from a direct extern property accessor");
        Assert(!module.Imports.Any(import => import.DispatchClass == "data_lane"),
            "a static direct extern property should not enter instance-setter fusion");
    }

    private static void HostAbiBudgetSplitsLargeGroups()
    {
        StringBuilder body = new();
        for (int index = 0; index < 8192; ++index)
        {
            body.Append("AvidScript.Generated.Set(target, ")
                .Append(index)
                .AppendLine(");");
        }

        GuestModule module = Lower(body.ToString(), "Scripts/DataLaneHostBudget.cs").Module;
        GuestInstruction[] instructions = MainFunction(module).Blocks
            .SelectMany(block => block.Instructions)
            .ToArray();
        GuestType bufferType = module.Types.Single(type =>
            type.Id.StartsWith(
                "type:__avidscript_internal.data_lane_command_buffer.4096",
                StringComparison.Ordinal));

        Assert(instructions.Count(instruction =>
                instruction.Op == "call"
                && module.Imports.Any(import =>
                    import.Id == instruction.TargetId
                    && import.Name == "avid_data_lane_submit")) == 2,
            "8192 generated setter calls should split into two host-budget submissions");
        Assert(bufferType.Size == 24 + (4096 * 32) && bufferType.Size <= 256 * 1024,
            "each fused buffer must stay within MaxCommands=4096 and MaxBytes=256KiB");
    }

    private static void FusionCanBeDisabledPerProfile()
    {
        const string body = """
            AvidScript.Generated.Set(target, 1);
            AvidScript.Generated.Set(target, 2);
            AvidScript.Generated.Set(target, 3);
            """;
        Lowered lowered = LowerSource(
            BuildSource(body),
            "Scripts/DataLaneDisabled.cs",
            new[]
            {
                new SemanticReferenceSource(
                    GeneratedDataLaneReferenceSource,
                    "generated://AvidScript.Bindings.generated.cs",
                    IsExecutable: true),
            },
            enableDataLaneFusion: false);
        GuestInstruction[] instructions = MainFunction(lowered.Module).Blocks
            .SelectMany(block => block.Instructions)
            .ToArray();

        Assert(!lowered.Module.Imports.Any(import => import.DispatchClass == "data_lane")
            && instructions.Count(instruction =>
                instruction.Op == "call"
                && instruction.TargetId?.Contains(".Set(", StringComparison.Ordinal) == true) == 3,
            "profile-disabled fusion should preserve ordinary generated extern calls");
    }

    private static Lowered Lower(string body, string sourceId)
    {
        return LowerSource(
            BuildSource(body),
            sourceId,
            new[]
            {
                new SemanticReferenceSource(
                    GeneratedDataLaneReferenceSource,
                    "generated://AvidScript.Bindings.generated.cs",
                    IsExecutable: true),
            });
    }

    private static Lowered LowerSource(
        string source,
        string sourceId,
        IReadOnlyList<SemanticReferenceSource>? referenceSources = null,
        bool enableDataLaneFusion = true)
    {
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        SemanticDocument semantic = SemanticAnalyzer.Analyze(
            source,
            sourceId,
            frontend.Source.Sha256,
            referenceSources ?? Array.Empty<SemanticReferenceSource>());
        Assert(semantic.Succeeded, $"{sourceId} should produce a valid semantic artifact");
        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(
            semantic,
            SemanticHash,
            enableDataLaneFusion);
        Assert(result.Succeeded && result.Module is not null,
            $"{sourceId} should lower to valid GuestIR: {string.Join("; ", result.Diagnostics.Select(item => item.Message))}");
        return new Lowered(semantic, result.Module!);
    }

    private static string BuildSource(string body)
    {
        return $$"""
            using System.Runtime.InteropServices;

            namespace Game
            {
                public static class Script
                {
                    [UnmanagedCallersOnly(EntryPoint = "avid_data_lane_test")]
                    public static void Main(AvidScript.Target target)
                    {
            {{body}}
                    }
                }
            }
            """;
    }

    private static GuestFunction MainFunction(GuestModule module)
    {
        return module.Functions.Single(function =>
            function.Id.Contains("global::Game.Script.Main", StringComparison.Ordinal));
    }

    private static GuestField Field(GuestType type, string name)
    {
        return type.Fields.Single(field => field.Name == name);
    }

    private static GuestInstruction StoreTo(
        GuestType type,
        IReadOnlyList<GuestInstruction> instructions,
        string fieldName)
    {
        string fieldId = Field(type, fieldName).Id;
        return instructions.Single(instruction =>
            instruction.Op == "field_store" && instruction.TargetId == fieldId);
    }

    private static uint StoredUInt32(
        GuestType type,
        IReadOnlyList<GuestInstruction> instructions,
        IReadOnlyDictionary<string, GuestInstruction> definitions,
        string fieldName)
    {
        GuestInstruction store = StoreTo(type, instructions, fieldName);
        return uint.Parse(definitions[store.OperandIds[1]].Constant!.Value!);
    }

    private static int StoredInt32(
        GuestType type,
        IReadOnlyList<GuestInstruction> instructions,
        IReadOnlyDictionary<string, GuestInstruction> definitions,
        string fieldName)
    {
        GuestInstruction store = StoreTo(type, instructions, fieldName);
        return int.Parse(definitions[store.OperandIds[1]].Constant!.Value!);
    }

    private static string Checksum(GuestModule module)
    {
        return Convert.ToHexString(SHA256.HashData(GuestIrSerializer.Serialize(module))).ToLowerInvariant();
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }

    private const string GeneratedDataLaneReferenceSource = """
        using System;
        using System.Runtime.InteropServices;

        namespace AvidScript
        {
            [AttributeUsage(AttributeTargets.Method)]
            public sealed class AvidScriptDataLaneAttribute : Attribute
            {
                public AvidScriptDataLaneAttribute(string optimizationClass, int bindingOrdinal) { }
            }

            public readonly struct Target
            {
                public readonly int Slot;
                public readonly int Generation;
            }

            public static class Generated
            {
                [AvidScriptDataLane("buffered_write", 17)]
                [DllImport("avidscript", EntryPoint = "avid_generated_property_set")]
                public static extern void Set(Target target, int value);
            }

            public static class Native
            {
                [DllImport("env", EntryPoint = "host_probe")]
                public static extern void Probe();
            }
        }
        """;

    private const string DataLaneAttributeReferenceSource = """
        using System;

        namespace AvidScript
        {
            [AttributeUsage(AttributeTargets.Method)]
            internal sealed class AvidScriptDataLaneAttribute : Attribute
            {
                internal AvidScriptDataLaneAttribute(string optimizationClass, int bindingOrdinal) { }
            }
        }
        """;

    private sealed record Lowered(SemanticDocument Semantic, GuestModule Module);
}
