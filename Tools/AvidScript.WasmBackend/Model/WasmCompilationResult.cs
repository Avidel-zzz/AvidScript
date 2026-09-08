using System.Collections.Generic;
using AvidScript.GuestIr;

namespace AvidScript.WasmBackend;

public sealed record WasmCompilationResult(
    bool Succeeded,
    byte[] Bytes,
    IReadOnlyList<GuestWasmDebugOffset> DebugOffsets,
    IReadOnlyList<WasmDiagnostic> Diagnostics,
    WasmCooperativeSafepointAttestation? CooperativeSafepointAttestation = null);

public sealed record WasmDiagnostic(
    string Code,
    string Severity,
    string Message);
