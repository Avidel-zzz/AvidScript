using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using AvidScript.GuestIr;

namespace AvidScript.WasmBackend;

public static class WasmModuleCompiler
{
    private static readonly byte[] Header =
    {
        0x00, 0x61, 0x73, 0x6d,
        0x01, 0x00, 0x00, 0x00,
    };

    public static WasmCompilationResult Compile(
        GuestModule module,
        WasmCompilationOptions? options = null)
    {
        ArgumentNullException.ThrowIfNull(module);
        GuestValidationResult validation = GuestModuleValidator.Validate(module);
        if (!validation.Succeeded)
        {
            WasmDiagnostic diagnostic = new(
                "ASWB1001",
                "error",
                "Guest IR validation failed before WASM compilation.");
            return new WasmCompilationResult(
                false,
                Array.Empty<byte>(),
                Array.Empty<GuestWasmDebugOffset>(),
                new[] { diagnostic });
        }

        try
        {
            WasmCompilationOptions effectiveOptions = options ?? new WasmCompilationOptions();
            WasmCooperativeSafepointPlan safepointPlan =
                WasmCooperativeSafepointPlan.Create(module, effectiveOptions);
            (byte[] bytes, IReadOnlyList<GuestWasmDebugOffset> offsets,
                WasmCooperativeSafepointAttestation? attestation) =
                CompileValidated(module, safepointPlan);
            return new WasmCompilationResult(
                true,
                bytes,
                offsets,
                Array.Empty<WasmDiagnostic>(),
                attestation);
        }
        catch (Exception exception) when (exception is NotSupportedException
            or InvalidOperationException
            or KeyNotFoundException
            or FormatException
            or OverflowException)
        {
            WasmDiagnostic diagnostic = new("ASWB1002", "error", exception.Message);
            return new WasmCompilationResult(
                false,
                Array.Empty<byte>(),
                Array.Empty<GuestWasmDebugOffset>(),
                new[] { diagnostic });
        }
    }

    private static (
        byte[] Bytes,
        IReadOnlyList<GuestWasmDebugOffset> DebugOffsets,
        WasmCooperativeSafepointAttestation? Attestation) CompileValidated(
        GuestModule module,
        WasmCooperativeSafepointPlan safepointPlan)
    {
        WasmModuleLayout layout = WasmModuleLayout.Create(module, safepointPlan);
        WasmBinaryWriter writer = new();
        List<GuestWasmDebugOffset> debugOffsets = new();
        writer.WriteBytes(Header);
        WriteProvenanceSection(writer, module);
        WriteSafepointProofSection(writer, module, safepointPlan);
        WriteTypeSection(writer, layout);
        WriteImportSection(writer, module, layout, safepointPlan);
        WriteFunctionSection(writer, module, layout);
        WriteMemorySection(writer, module, layout);
        WriteGlobalSection(writer, module, safepointPlan);
        WriteExportSection(writer, module, layout);
        int emittedSafepointCount = WriteCodeSection(
            writer,
            module,
            layout,
            safepointPlan,
            debugOffsets);
        WriteDataSection(writer, module, layout);
        return (
            writer.ToArray(),
            debugOffsets,
            safepointPlan.CreateAttestation(emittedSafepointCount));
    }

    private static void WriteSafepointProofSection(
        WasmBinaryWriter writer,
        GuestModule module,
        WasmCooperativeSafepointPlan safepointPlan)
    {
        if (!safepointPlan.Enabled)
        {
            return;
        }
        writer.WriteSection(0, section =>
        {
            section.WriteName("avidscript.safepoints");
            string payload = string.Join(
                (char)10,
                "schema=2",
                "mode=bounded_counter_v1",
                $"poll_interval={safepointPlan.Interval}",
                $"import={WasmCooperativeSafepointPlan.ImportModule}.{WasmCooperativeSafepointPlan.ImportName}",
                $"loop_poll_blocks={safepointPlan.LoopPollCount}",
                $"recursive_functions={safepointPlan.RecursiveFunctionCount}",
                $"site_count={safepointPlan.SiteCount}",
                $"site_sha256={safepointPlan.SiteSha256}",
                "coverage=cfg_feedback_edges_and_recursive_entries",
                $"guest_ir={module.SchemaVersion}/{module.IrVersion}");
            section.WriteBytes(Encoding.UTF8.GetBytes(payload));
        });
    }

    private static void WriteProvenanceSection(WasmBinaryWriter writer, GuestModule module)
    {
        writer.WriteSection(0, section =>
        {
            section.WriteName("avidscript.provenance");
            string payload = string.Join(
                (char)10,
                $"module_id={module.ModuleId}",
                $"source_id={module.Provenance.SourceId}",
                $"source_sha256={module.Provenance.SourceSha256}",
                $"frontend_sha256={module.Provenance.FrontendSha256}",
                $"semantic_sha256={module.Provenance.SemanticSha256}",
                $"guest_ir={module.SchemaVersion}/{module.IrVersion}");
            section.WriteBytes(Encoding.UTF8.GetBytes(payload));
        });
    }

    private static void WriteTypeSection(
        WasmBinaryWriter writer,
        WasmModuleLayout layout)
    {
        writer.WriteSection(1, section =>
        {
            section.WriteU32(checked((uint)layout.Signatures.Count));
            foreach (WasmFunctionSignature signature in layout.Signatures)
            {
                section.WriteByte(0x60);
                section.WriteU32(checked((uint)signature.Parameters.Count));
                foreach (WasmValueType parameter in signature.Parameters)
                {
                    section.WriteByte((byte)parameter);
                }

                if (signature.Result is null)
                {
                    section.WriteU32(0);
                }
                else
                {
                    section.WriteU32(1);
                    section.WriteByte((byte)signature.Result.Value);
                }
            }
        });
    }

    private static void WriteImportSection(
        WasmBinaryWriter writer,
        GuestModule module,
        WasmModuleLayout layout,
        WasmCooperativeSafepointPlan safepointPlan)
    {
        if (module.Imports.Count == 0 && !safepointPlan.Enabled)
        {
            return;
        }

        writer.WriteSection(2, section =>
        {
            section.WriteU32(checked(
                (uint)module.Imports.Count + (safepointPlan.Enabled ? 1u : 0u)));
            if (safepointPlan.Enabled)
            {
                section.WriteName(WasmCooperativeSafepointPlan.ImportModule);
                section.WriteName(WasmCooperativeSafepointPlan.ImportName);
                section.WriteByte(0x00);
                section.WriteU32(
                    layout.TypeIndices[WasmCooperativeSafepointPlan.ImportId]);
            }
            foreach (GuestImport import in module.Imports)
            {
                section.WriteName(import.Module);
                section.WriteName(import.Name);
                section.WriteByte(0x00);
                section.WriteU32(layout.TypeIndices[import.Id]);
            }
        });
    }

    private static void WriteFunctionSection(
        WasmBinaryWriter writer,
        GuestModule module,
        WasmModuleLayout layout)
    {
        writer.WriteSection(3, section =>
        {
            section.WriteU32(checked((uint)module.Functions.Count));
            foreach (GuestFunction function in module.Functions)
            {
                section.WriteU32(layout.TypeIndices[function.Id]);
            }
        });
    }

    private static void WriteMemorySection(
        WasmBinaryWriter writer,
        GuestModule module,
        WasmModuleLayout layout)
    {
        const uint wasmPageSize = 65536;
        uint requiredBytes = checked((uint)Math.Max(module.MemoryLayout.HeapStart, 1));
        uint pageCount = checked((requiredBytes + wasmPageSize - 1) / wasmPageSize);
        bool requiresRuntimeStack = module.Functions.Any(
            function => WasmFunctionFrameLayout.Create(function, layout).FrameSize > 0);
        if (requiresRuntimeStack)
        {
            pageCount = checked(pageCount + 1);
        }

        writer.WriteSection(5, section =>
        {
            section.WriteU32(1);
            section.WriteByte(0x00);
            section.WriteU32(Math.Max(pageCount, 1));
        });
    }

    private static void WriteGlobalSection(
        WasmBinaryWriter writer,
        GuestModule module,
        WasmCooperativeSafepointPlan safepointPlan)
    {
        writer.WriteSection(6, section =>
        {
            section.WriteU32(safepointPlan.Enabled ? 2u : 1u);
            section.WriteByte((byte)WasmValueType.I32);
            section.WriteByte(0x01);
            section.WriteByte(0x41);
            section.WriteS32(module.MemoryLayout.HeapStart);
            section.WriteByte(0x0b);
            if (safepointPlan.Enabled)
            {
                section.WriteByte((byte)WasmValueType.I32);
                section.WriteByte(0x01);
                section.WriteByte(0x41);
                section.WriteS32(checked((int)safepointPlan.Interval));
                section.WriteByte(0x0b);
            }
        });
    }

    private static void WriteExportSection(
        WasmBinaryWriter writer,
        GuestModule module,
        WasmModuleLayout layout)
    {
        writer.WriteSection(7, section =>
        {
            section.WriteU32(checked((uint)(module.Exports.Count + 1)));
            section.WriteName("memory");
            section.WriteByte(0x02);
            section.WriteU32(0);
            foreach (GuestExport export in module.Exports)
            {
                section.WriteName(export.Name);
                section.WriteByte(0x00);
                section.WriteU32(layout.FunctionIndices[export.FunctionId]);
            }
        });
    }

    private static int WriteCodeSection(
        WasmBinaryWriter writer,
        GuestModule module,
        WasmModuleLayout layout,
        WasmCooperativeSafepointPlan safepointPlan,
        ICollection<GuestWasmDebugOffset> debugOffsets)
    {
        int emittedSafepointCount = 0;
        writer.WriteSection(10, section =>
        {
            section.WriteU32(checked((uint)module.Functions.Count));
            foreach (GuestFunction function in module.Functions)
            {
                WasmFunctionCompilationResult body =
                    new WasmFunctionCompiler(
                        module,
                        function,
                        layout,
                        safepointPlan).Compile();
                emittedSafepointCount = checked(
                    emittedSafepointCount + body.CooperativeSafepointCount);
                int functionIndex = checked((int)layout.FunctionIndices[function.Id]);
                foreach (WasmFunctionInstructionOffset offset in body.InstructionOffsets)
                {
                    debugOffsets.Add(new GuestWasmDebugOffset(
                        functionIndex,
                        offset.GuestInstructionId,
                        offset.FunctionOffset));
                }
                section.WriteU32(checked((uint)body.Bytes.Length));
                section.WriteBytes(body.Bytes);
            }
        });
        return emittedSafepointCount;
    }

    private static void WriteDataSection(
        WasmBinaryWriter writer,
        GuestModule module,
        WasmModuleLayout layout)
    {
        List<WasmDataInitializer> initializers = new();
        foreach (GuestGlobal global in module.Globals)
        {
            GuestStateSlot slot = module.MemoryLayout.StateSlots.Single(
                item => string.Equals(item.GlobalId, global.Id, StringComparison.Ordinal));
            if (!GuestConstantCodec.TryEncode(
                global.InitialValue,
                layout.Types[global.TypeId],
                out byte[] bytes))
            {
                throw new InvalidOperationException(
                    $"Global '{global.Id}' has no canonical initializer encoding.");
            }

            if (bytes.Any(value => value != 0))
            {
                initializers.Add(new WasmDataInitializer(
                    global.Id,
                    slot.Offset,
                    bytes));
            }
        }

        foreach (GuestDataSegment segment in module.DataSegments)
        {
            initializers.Add(new WasmDataInitializer(
                segment.Id,
                segment.Address,
                segment.Bytes.ToArray()));
        }

        WasmDataInitializer[] ordered = initializers
            .OrderBy(item => item.Address)
            .ThenBy(item => item.Id, StringComparer.Ordinal)
            .ToArray();
        if (ordered.Length == 0)
        {
            return;
        }

        writer.WriteSection(11, section =>
        {
            section.WriteU32(checked((uint)ordered.Length));
            foreach (WasmDataInitializer initializer in ordered)
            {
                section.WriteU32(0);
                section.WriteByte(0x41);
                section.WriteS32(initializer.Address);
                section.WriteByte(0x0b);
                section.WriteU32(checked((uint)initializer.Bytes.Length));
                section.WriteBytes(initializer.Bytes);
            }
        });
    }

    private sealed record WasmDataInitializer(
        string Id,
        int Address,
        byte[] Bytes);
}
