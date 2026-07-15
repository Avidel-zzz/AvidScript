using System.Collections.Generic;

namespace AvidScript.WasmBackend;

public sealed record WasmArtifactInfo(
    IReadOnlyList<byte> SectionIds,
    IReadOnlyList<WasmImportInfo> Imports,
    IReadOnlyList<WasmExportInfo> Exports,
    IReadOnlyList<WasmCustomSectionInfo> CustomSections,
    uint FunctionBodyCount,
    uint DataSegmentCount,
    uint BoundsTrapCount);

public sealed record WasmImportInfo(string Module, string Name, byte Kind, uint TypeIndex);

public sealed record WasmExportInfo(string Name, byte Kind, uint Index);

public sealed record WasmCustomSectionInfo(string Name, string PayloadText);