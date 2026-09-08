using System.Collections.Generic;

namespace AvidScript.WasmBackend;

internal sealed record WasmFunctionCompilationResult(
    byte[] Bytes,
    IReadOnlyList<WasmFunctionInstructionOffset> InstructionOffsets,
    int CooperativeSafepointCount);

internal sealed record WasmFunctionInstructionOffset(
    string GuestInstructionId,
    int FunctionOffset);
