using System;
using System.Buffers.Binary;
using AvidScript.GuestIr;

namespace AvidScript.WasmBackend;

internal static class WasmFramedExportCompiler
{
    public static byte[] Compile(WasmFramedExport export, WasmModuleLayout module)
    {
        GuestCallFrameLayout frame = export.Layout;
        WasmBinaryWriter body = new(); body.WriteU32(0); // parameters: address, byte count; no locals
        Local(body, 1); I32(body, frame.ByteSize); body.WriteByte(0x47); Reject(body);
        Local(body, 0); I32(body, GuestCallFrameLayout.Alignment - 1); body.WriteByte(0x71); Reject(body);
        Local(body, 0); I32(body, module.ManagedHeap.StackStart); body.WriteByte(0x49); Reject(body);
        // Widen before adding. Require an active caller's reserved stack range and
        // check memory bounds before inspecting any header byte.
        End64(body, frame.ByteSize); body.WriteByte(0x23); body.WriteU32(0); body.WriteByte(0xad);
        body.WriteByte(0x56); Reject(body);
        End64(body, frame.ByteSize); body.WriteByte(0x3f); body.WriteByte(0); body.WriteByte(0xad);
        body.WriteByte(0x42); body.WriteS64(16); body.WriteByte(0x86); body.WriteByte(0x56); Reject(body);
        Check(0, unchecked((int)GuestCallFrameLayout.Magic)); Check(4, GuestCallFrameLayout.Version);
        Check(8, frame.ByteSize); Check(12, 0);
        byte[] signature = Convert.FromHexString(frame.SignatureSha256);
        for (int i = 0; i < signature.Length; i += 4) Check(16 + i, BinaryPrimitives.ReadInt32LittleEndian(signature.AsSpan(i)));
        for (int i = 48; i < GuestCallFrameLayout.HeaderBytes; i += 4) Check(i, 0);

        GuestType result = frame.Result.Type;
        // Address is sret for aggregates, or destination of the following scalar store.
        if (result.Storage != "none") Address(body, frame.Result.Offset);
        foreach (GuestCallFrameSlot parameter in frame.Parameters)
        {
            Address(body, parameter.Offset);
            if (parameter.Type.Storage != "memory") WasmMemoryEmitter.WriteLoad(body, parameter.Type);
        }
        body.WriteByte(0x10); body.WriteU32(module.FunctionIndices[export.Function.Id]);
        if (result.Storage is not ("none" or "memory")) WasmMemoryEmitter.WriteStore(body, result);

        foreach (GuestCallFrameRoot root in frame.Roots)
        {
            StoreI32(frame.ScratchOffset, unchecked((int)GuestManagedHeap.Magic));
            StoreI32(frame.ScratchOffset + 4, (int)GuestManagedHeapCommand.SetRoot);
            Address(body, frame.ScratchOffset + 8); Address(body, root.TokenOffset); Load64(body); Store64(body);
            Address(body, frame.ScratchOffset + 16);
            if (root.ParameterIndex < 0) { Address(body, frame.Result.Offset + root.ValueOffset); Load64(body); }
            else
            {
                int descriptor = frame.Parameters[root.ParameterIndex].Offset;
                Address(body, descriptor + GuestBorrowedReference.OwnerOffset); Load64(body); body.WriteByte(0x50);
                Address(body, descriptor + GuestBorrowedReference.AddressOffset); Load32(body); body.WriteByte(0x45); body.WriteByte(0x45);
                body.WriteByte(0x71); body.WriteByte(0x04); body.WriteByte(0x7e);
                Address(body, descriptor + GuestBorrowedReference.AddressOffset); Load32(body);
                I32(body, root.ValueOffset); body.WriteByte(0x6a); Load64(body);
                body.WriteByte(0x05); body.WriteByte(0x42); body.WriteS64(0); body.WriteByte(0x0b);
            }
            Store64(body);
            Address(body, frame.ScratchOffset); I32(body, 24); I32(body, 0); I32(body, 0);
            body.WriteByte(0x10); body.WriteU32(module.FunctionIndices[module.ManagedHeap.ImportId!]);
            body.WriteByte(0x45); Reject(body);
        }
        body.WriteByte(0x0b); return body.ToArray();

        void Check(int offset, int value)
        { Address(body, offset); Load32(body); I32(body, value); body.WriteByte(0x47); Reject(body); }
        void StoreI32(int offset, int value)
        { Address(body, offset); I32(body, value); body.WriteByte(0x36); body.WriteU32(2); body.WriteU32(0); }
    }

    private static void Local(WasmBinaryWriter writer, uint index) { writer.WriteByte(0x20); writer.WriteU32(index); }
    private static void I32(WasmBinaryWriter writer, int value) { writer.WriteByte(0x41); writer.WriteS32(value); }
    private static void Address(WasmBinaryWriter writer, int offset) { Local(writer, 0); I32(writer, offset); writer.WriteByte(0x6a); }
    private static void End64(WasmBinaryWriter writer, int size)
    { Local(writer, 0); writer.WriteByte(0xad); writer.WriteByte(0x42); writer.WriteS64(size); writer.WriteByte(0x7c); }
    private static void Load32(WasmBinaryWriter writer) { writer.WriteByte(0x28); writer.WriteU32(2); writer.WriteU32(0); }
    private static void Load64(WasmBinaryWriter writer) { writer.WriteByte(0x29); writer.WriteU32(0); writer.WriteU32(0); }
    private static void Store64(WasmBinaryWriter writer) { writer.WriteByte(0x37); writer.WriteU32(0); writer.WriteU32(0); }
    private static void Reject(WasmBinaryWriter writer)
    { writer.WriteByte(0x04); writer.WriteByte(0x40); writer.WriteByte(0x00); writer.WriteByte(0x0b); }
}
