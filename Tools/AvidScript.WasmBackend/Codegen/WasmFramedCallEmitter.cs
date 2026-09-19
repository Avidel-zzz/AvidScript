using System;
using System.Buffers.Binary;
using System.Linq;
using AvidScript.GuestIr;

namespace AvidScript.WasmBackend;

internal sealed partial class WasmFunctionCompiler
{
    private void CompileFramedCall(WasmBinaryWriter body, GuestInstruction instruction)
    {
        FlushArrayRegion(body);
        WasmFramedExport export = moduleLayout.FramedExports[instruction.TargetId!];
        GuestCallFrameLayout call = export.Layout;
        void Address(WasmBinaryWriter writer, int offset = 0)
        {
            WriteLocalGet(writer, frameBaseLocalIndex);
            WriteI32Constant(writer, checked(frame.CallScratchOffset!.Value + offset)); writer.WriteByte(0x6a);
        }
        void I32(int offset, int value)
        {
            Address(body, offset); WriteI32Constant(body, value);
            body.WriteByte(0x36); body.WriteU32(2); body.WriteU32(0);
        }
        WasmMemoryEmitter.WriteZero(body, writer => Address(writer), GuestCallFrameLayout.HeaderBytes);
        I32(0, unchecked((int)GuestCallFrameLayout.Magic)); I32(4, GuestCallFrameLayout.Version); I32(8, call.ByteSize);
        byte[] signature = Convert.FromHexString(call.SignatureSha256);
        for (int i = 0; i < signature.Length; i += 4) I32(16 + i, BinaryPrimitives.ReadInt32LittleEndian(signature.AsSpan(i)));
        for (int i = 0; i < call.Parameters.Count; ++i)
        {
            GuestCallFrameSlot slot = call.Parameters[i]; string value = instruction.OperandIds[i];
            if (slot.Type.Storage == "memory")
                WasmMemoryEmitter.WriteCopy(body, writer => Address(writer, slot.Offset),
                    writer => WriteLocalGet(writer, localIndices[value]), slot.Type.Size);
            else
            {
                Address(body, slot.Offset); WriteLocalGet(body, localIndices[value]);
                WasmMemoryEmitter.WriteStore(body, slot.Type);
            }
        }
        // Roots belong to this activation. The adapter updates them before returning
        // through a host bridge that may collect. Ref/out retains the original location.
        foreach (GuestCallFrameRoot slot in call.Roots)
        {
            string value = slot.ParameterIndex < 0 ? instruction.ResultId! : instruction.OperandIds[slot.ParameterIndex];
            ManagedRoot root = managedRoots.Single(root => root.RegisterId == value
                && root.Offset == slot.ValueOffset && root.BorrowedPointee == (slot.ParameterIndex >= 0));
            Address(body, slot.TokenOffset); WriteLocalGet(body, root.LocalIndex);
            body.WriteByte(0x37); body.WriteU32(3); body.WriteU32(0);
        }
        Address(body); WriteI32Constant(body, call.ByteSize);
        body.WriteByte(0x10); body.WriteU32(export.FunctionIndex);
        if (instruction.ResultId is not { } result) return;
        if (call.Result.Type.Storage == "memory")
            WasmMemoryEmitter.WriteCopy(body, writer => WriteLocalGet(writer, localIndices[result]),
                writer => Address(writer, call.Result.Offset), call.Result.Type.Size);
        else
        {
            Address(body, call.Result.Offset); WasmMemoryEmitter.WriteLoad(body, call.Result.Type);
            WriteResult(body, instruction);
        }
    }
}
