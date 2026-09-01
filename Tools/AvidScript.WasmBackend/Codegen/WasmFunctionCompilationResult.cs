using System.Collections.Generic;

namespace AvidScript.WasmBackend;

internal sealed record WasmFunctionCompilationResult(
    byte[] Bytes,
    IReadOnlyList<WasmFunctionInstructionOffset> InstructionOffsets);

internal sealed record WasmFunctionInstructionOffset(
    string GuestInstructionId,
    int FunctionOffset);
