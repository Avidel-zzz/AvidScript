using System;
using System.Linq;
using AvidScript.GuestIr;

namespace AvidScript.WasmBackend;

internal sealed partial class WasmFunctionCompiler
{
    private void WriteBorrowOwner(WasmBinaryWriter body, string id)
    {
        WriteLocalGet(body, localIndices[id]); body.WriteByte(0x29); body.WriteU32(0); body.WriteU32(GuestBorrowedReference.OwnerOffset);
    }

    private void WriteBorrowI32(WasmBinaryWriter body, string id, int offset)
    {
        WriteLocalGet(body, localIndices[id]); body.WriteByte(0x28); body.WriteU32(0); body.WriteU32((uint)offset);
    }

    private void StoreBorrowI32(WasmBinaryWriter body, string id, int offset, Action<WasmBinaryWriter> value)
    {
        WriteLocalGet(body, localIndices[id]); value(body); body.WriteByte(0x36); body.WriteU32(0); body.WriteU32((uint)offset);
    }

    private void RequireBorrowAddress(WasmBinaryWriter body, string id)
    {
        WriteBorrowOwner(body, id); body.WriteByte(0x50);
        WriteBorrowI32(body, id, GuestBorrowedReference.AddressOffset); body.WriteByte(0x45);
        body.WriteByte(0x71); body.WriteByte(0x04); body.WriteByte(0x40); body.WriteByte(0x00); body.WriteByte(0x0b);
    }

    private void CompileBorrowedInstruction(WasmBinaryWriter body, GuestInstruction instruction)
    {
        string? result = instruction.ResultId;
        if (instruction.Op == "borrow_global")
        {
            WasmMemoryEmitter.WriteZero(body, writer => WriteLocalGet(writer, localIndices[result!]), GuestBorrowedReference.Size);
            StoreBorrowI32(body, result!, GuestBorrowedReference.AddressOffset,
                writer => WriteI32Constant(writer, GetStateSlot(instruction.TargetId!).Offset));
            return;
        }
        if (instruction.Op == "borrow_address")
        {
            string storage = instruction.TargetId!;
            WasmMemoryEmitter.WriteZero(body, writer => WriteLocalGet(writer, localIndices[result!]), GuestBorrowedReference.Size);
            StoreBorrowI32(body, result!, GuestBorrowedReference.AddressOffset, writer =>
            {
                if (IsMemoryValue(storage)) WriteLocalGet(writer, localIndices[storage]); else WriteFrameAddress(writer, storage);
            });
            return;
        }
        string source = instruction.OperandIds[0];
        if (instruction.Op == "borrow_managed")
        {
            GuestField field = moduleLayout.Types[GetValueType(source).ElementTypeId!].Fields.Single(item => item.Id == instruction.TargetId);
            WriteLocalGet(body, localIndices[source]); body.WriteByte(0x50);
            body.WriteByte(0x04); body.WriteByte(0x40); body.WriteByte(0x00); body.WriteByte(0x0b);
            WasmMemoryEmitter.WriteZero(body, writer => WriteLocalGet(writer, localIndices[result!]), GuestBorrowedReference.Size);
            WriteLocalGet(body, localIndices[result!]); WriteLocalGet(body, localIndices[source]);
            body.WriteByte(0x37); body.WriteU32(0); body.WriteU32(0);
            StoreBorrowI32(body, result!, GuestBorrowedReference.HeapOffset, writer => WriteI32Constant(writer, field.Offset));
            return;
        }
        RequireBorrowAddress(body, source);
        if (instruction.Op == "borrow_field")
        {
            GuestField field = moduleLayout.Types[GetValueType(source).ElementTypeId!].Fields.Single(item => item.Id == instruction.TargetId);
            WriteMemoryCopy(body, result!, source, GuestBorrowedReference.Size);
            WriteBorrowOwner(body, source); body.WriteByte(0x50); body.WriteByte(0x04); body.WriteByte(0x40);
            Advance(GuestBorrowedReference.AddressOffset); body.WriteByte(0x05); Advance(GuestBorrowedReference.HeapOffset); body.WriteByte(0x0b);
            return;
            void Advance(int offset) => StoreBorrowI32(body, result!, offset, writer =>
            { WriteBorrowI32(writer, source, offset); WriteI32Constant(writer, field.Offset); writer.WriteByte(0x6a); });
        }

        bool read = instruction.Op == "borrow_load";
        string value = read ? result! : instruction.OperandIds[1];
        GuestType valueType = moduleLayout.Types[instruction.TargetId!];
        WriteBorrowOwner(body, source); body.WriteByte(0x50); body.WriteByte(0x04); body.WriteByte(0x40);
        if (valueType.Storage == "memory")
        {
            Action<WasmBinaryWriter> storage = writer => WriteBorrowI32(writer, source, GuestBorrowedReference.AddressOffset);
            Action<WasmBinaryWriter> local = writer => WriteLocalGet(writer, localIndices[value]);
            WasmMemoryEmitter.WriteCopy(body, read ? local : storage, read ? storage : local, valueType.Size);
        }
        else
        {
            WriteBorrowI32(body, source, GuestBorrowedReference.AddressOffset);
            if (read) { WasmMemoryEmitter.WriteLoad(body, valueType); WriteResult(body, instruction); }
            else { WriteLocalGet(body, localIndices[value]); WasmMemoryEmitter.WriteStore(body, valueType); }
        }
        body.WriteByte(0x05);
        if (read && valueType.Storage == "memory")
            WasmMemoryEmitter.WriteZero(body, writer => WriteLocalGet(writer, localIndices[value]), valueType.Size);
        foreach (GuestManagedLeaf leaf in GuestManagedHeap.Leaves(moduleLayout.Types, valueType.Id))
        {
            bool edge = leaf.Type.Kind == "managed_ref";
            ManagedHeader(body, edge ? (read ? GuestManagedHeapCommand.ReadReference : GuestManagedHeapCommand.WriteReference)
                : (read ? GuestManagedHeapCommand.ReadBytes : GuestManagedHeapCommand.WriteBytes));
            ManagedStoreToken(body, 8, writer => WriteBorrowOwner(writer, source));
            ManagedStoreI32(body, 16, 0);
            ManagedScratchAddress(body, 20); WriteBorrowI32(body, source, GuestBorrowedReference.HeapOffset);
            WriteI32Constant(body, leaf.Offset); body.WriteByte(0x6a); body.WriteByte(0x36); body.WriteU32(0); body.WriteU32(0);
            int input = edge ? 24 : 28;
            if (!edge) ManagedStoreI32(body, 24, leaf.Type.Size);
            if (!read)
            {
                ManagedScratchAddress(body, edge ? 24 : 28); WriteLocalGet(body, localIndices[value]);
                if (valueType.Storage == "memory")
                { WriteI32Constant(body, leaf.Offset); body.WriteByte(0x6a); WasmMemoryEmitter.WriteLoad(body, leaf.Type); }
                WasmMemoryEmitter.WriteStore(body, leaf.Type); input += leaf.Type.Size;
            }
            ManagedPacketCall(body, input, read ? leaf.Type.Size : 0);
            if (read)
            {
                if (valueType.Storage == "memory")
                { WriteLocalGet(body, localIndices[value]); WriteI32Constant(body, leaf.Offset); body.WriteByte(0x6a); }
                ManagedScratchAddress(body, 48); WasmMemoryEmitter.WriteLoad(body, leaf.Type);
                if (valueType.Storage == "memory") WasmMemoryEmitter.WriteStore(body, leaf.Type); else WriteResult(body, instruction);
            }
        }
        body.WriteByte(0x0b);
    }
}
