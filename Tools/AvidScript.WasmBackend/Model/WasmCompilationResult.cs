using System.Collections.Generic;

namespace AvidScript.WasmBackend;

public sealed record WasmCompilationResult(
    bool Succeeded,
    byte[] Bytes,
    IReadOnlyList<WasmDiagnostic> Diagnostics);

public sealed record WasmDiagnostic(
    string Code,
    string Severity,
    string Message);
