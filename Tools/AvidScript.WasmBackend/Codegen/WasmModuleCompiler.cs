using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Text.Json;
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
        WriteLanguageErrorCatalogSection(writer, module);
        WriteCallFrameSection(writer, layout);
        WriteHostCallFrameSection(writer, module, layout);
        WriteSafepointProofSection(writer, module, safepointPlan);
        WriteTypeSection(writer, layout);
        WriteImportSection(writer, module, layout, safepointPlan);
        WriteFunctionSection(writer, module, layout);
        WriteTableSection(writer, layout);
        WriteMemorySection(writer, module, layout);
        WriteGlobalSection(writer, module, layout, safepointPlan);
        WriteExportSection(writer, module, layout);
        WriteElementSection(writer, layout);
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

    private static void WriteCallFrameSection(WasmBinaryWriter writer, WasmModuleLayout layout)
    {
        if (layout.FramedExports.Count == 0) return;
        writer.WriteSection(0, section =>
        {
            section.WriteName(GuestCallFrameLayout.SectionName);
            section.WriteBytes(JsonSerializer.SerializeToUtf8Bytes(new
            {
                schema_version = GuestCallFrameLayout.Version,
                exports = layout.FramedExports.Values.Select(export => new
                {
                    name = export.Contract.Name,
                    signature_sha256 = export.Layout.SignatureSha256,
                    frame_bytes = export.Layout.ByteSize,
                }).ToArray(),
            }));
        });
    }

    private static void WriteHostCallFrameSection(WasmBinaryWriter writer, GuestModule module, WasmModuleLayout layout)
    {
        WasmFramedExport[] routes = layout.FramedExports.Values.Where(export => export.Contract.HostImportId is not null).ToArray();
        if (routes.Length == 0) return;
        writer.WriteSection(0, section =>
        {
            section.WriteName(GuestCallFrameLayout.HostSectionName);
            section.WriteBytes(JsonSerializer.SerializeToUtf8Bytes(new
            {
                schema_version = routes.Any(export => export.Contract.HostDispatchTargets is not null)
                    ? GuestCallFrameLayout.HostDispatchSectionVersion : GuestCallFrameLayout.HostSectionVersion,
                exports = routes.Select(export =>
                {
                    GuestImport import = module.Imports.Single(item => item.Id == export.Contract.HostImportId);
                    return new
                    {
                        name = export.Contract.Name,
                        import_module = import.Module,
                        import_name = import.Name,
                        signature_sha256 = export.Layout.SignatureSha256,
                        frame_bytes = export.Layout.ByteSize,
                        parameter_offsets = export.Layout.Parameters.Select(slot => slot.Offset).ToArray(),
                        root_token_offsets = export.Layout.Roots.Select(slot => slot.TokenOffset).ToArray(),
                        dispatch_targets = export.Contract.HostDispatchTargets?.Select(target => new
                        {
                            selector = target.Selector,
                            export_name = target.ExportName,
                        }).ToArray(),
                    };
                }).ToArray(),
            }, new JsonSerializerOptions { DefaultIgnoreCondition = System.Text.Json.Serialization.JsonIgnoreCondition.WhenWritingNull }));
        });
    }

    private static void WriteTableSection(WasmBinaryWriter writer, WasmModuleLayout layout)
    {
        if (layout.FunctionReferences.Count == 0) return;
        writer.WriteSection(4, section =>
        {
            section.WriteU32(1);
            section.WriteByte(0x70); // funcref; private, fixed-size WASM 1.0 table.
            section.WriteByte(1);
            uint size = checked((uint)layout.TableFunctionIds.Count + 1);
            section.WriteU32(size);
            section.WriteU32(size);
        });
    }

    private static void WriteElementSection(WasmBinaryWriter writer, WasmModuleLayout layout)
    {
        if (layout.TableFunctionIds.Count == 0) return;
        writer.WriteSection(9, section =>
        {
            section.WriteU32(1);
            section.WriteU32(0); // active element segment for table zero
            section.WriteByte(0x41);
            section.WriteS32(1); // reserve the null slot
            section.WriteByte(0x0b);
            section.WriteU32(checked((uint)layout.TableFunctionIds.Count));
            foreach (string target in layout.TableFunctionIds) section.WriteU32(layout.FunctionIndices[target]);
        });
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
            if (module.TaskLocalLifetimes is { } lifetimes)
                payload += "\ntask_local_exception_model=" + lifetimes.ExceptionModel;
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
            section.WriteU32(checked((uint)(module.Functions.Count + layout.FramedExports.Count)));
            foreach (GuestFunction function in module.Functions)
            {
                section.WriteU32(layout.TypeIndices[function.Id]);
            }
            foreach (WasmFramedExport export in layout.FramedExports.Values) section.WriteU32(export.SignatureIndex);
        });
    }

    private static void WriteMemorySection(
        WasmBinaryWriter writer,
        GuestModule module,
        WasmModuleLayout layout)
    {
        const uint wasmPageSize = 65536;
        uint requiredBytes = checked((uint)Math.Max(layout.ManagedHeap.StackStart, 1));
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
        WasmModuleLayout layout,
        WasmCooperativeSafepointPlan safepointPlan)
    {
        writer.WriteSection(6, section =>
        {
            section.WriteU32((safepointPlan.Enabled ? 2u : 1u) + (layout.ManagedHeap.Enabled ? 1u : 0u));
            section.WriteByte((byte)WasmValueType.I32);
            section.WriteByte(0x01);
            section.WriteByte(0x41);
            section.WriteS32(layout.ManagedHeap.StackStart);
            section.WriteByte(0x0b);
            if (safepointPlan.Enabled)
            {
                section.WriteByte((byte)WasmValueType.I32);
                section.WriteByte(0x01);
                section.WriteByte(0x41);
                section.WriteS32(checked((int)safepointPlan.Interval));
                section.WriteByte(0x0b);
            }
            if (layout.ManagedHeap.Enabled)
            {
                section.WriteByte((byte)WasmValueType.I32); section.WriteByte(1);
                section.WriteByte(0x41); section.WriteS32(0); section.WriteByte(0x0b);
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
            section.WriteU32(checked((uint)(module.Exports.Count + layout.FramedExports.Count + 1)));
            section.WriteName("memory");
            section.WriteByte(0x02);
            section.WriteU32(0);
            foreach (GuestExport export in module.Exports)
            {
                section.WriteName(export.Name);
                section.WriteByte(0x00);
                section.WriteU32(layout.FunctionIndices[export.FunctionId]);
            }
            foreach (WasmFramedExport export in layout.FramedExports.Values)
            {
                section.WriteName(export.Contract.Name); section.WriteByte(0);
                section.WriteU32(export.FunctionIndex);
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
            section.WriteU32(checked((uint)(module.Functions.Count + layout.FramedExports.Count)));
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
            foreach (WasmFramedExport export in layout.FramedExports.Values)
            {
                byte[] body = WasmFramedExportCompiler.Compile(export, layout);
                section.WriteU32(checked((uint)body.Length)); section.WriteBytes(body);
            }
        });
        return emittedSafepointCount;
    }

    private static void WriteLanguageErrorCatalogSection(WasmBinaryWriter writer, GuestModule module)
    {
        if (module.LanguageErrorCatalog is not { } catalog) return;

        byte[] payload = JsonSerializer.SerializeToUtf8Bytes(new
        {
            schema_version = 1,
            guest_ir_schema_version = module.SchemaVersion,
            guest_ir_version = module.IrVersion,
            module_id = module.ModuleId,
            source_sha256 = module.Provenance.SourceSha256,
            types = catalog.Types.Select(entry => new
            {
                token = entry.Token,
                type_id = entry.TypeId,
            }).ToArray(),
            sources = catalog.Sources.Select(entry => new
            {
                token = entry.Token,
                source_id = entry.SourceId,
                source_length = entry.SourceLength,
                start = entry.Start,
                length = entry.Length,
                line = entry.Line,
                column = entry.Column,
                end_line = entry.EndLine,
                end_column = entry.EndColumn,
            }).ToArray(),
        });
        if (payload.Length > 4 * 1024 * 1024)
            throw new InvalidOperationException("Language-error WASM metadata exceeds 4 MiB.");

        writer.WriteSection(0, section =>
        {
            section.WriteName("avidscript.language_errors");
            section.WriteBytes(payload);
        });
    }

    private static void WriteDataSection(
        WasmBinaryWriter writer,
        GuestModule module,
        WasmModuleLayout layout)
    {
        List<WasmDataInitializer> initializers = new();
        if (layout.ManagedHeap.Enabled)
            initializers.Add(new WasmDataInitializer("$managed_heap_configuration", layout.ManagedHeap.ConfigurationAddress, layout.ManagedHeap.Configuration));
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
