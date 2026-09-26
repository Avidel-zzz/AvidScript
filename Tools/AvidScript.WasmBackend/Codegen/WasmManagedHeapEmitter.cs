using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.GuestIr;

namespace AvidScript.WasmBackend;

internal sealed partial class WasmFunctionCompiler
{
    private sealed record ManagedRoot(string RegisterId, int Offset, bool Aggregate, uint LocalIndex, bool BorrowedPointee = false);
    private readonly List<ManagedRoot> managedRoots = new();
    private uint managedFrameLocal;
    private bool HasManagedScope => frame.ManagedScratchOffset.HasValue;

    private void WriteManagedLocals(List<WasmValueType> locals, ref uint next)
    {
        if (!HasManagedScope) return;
        managedFrameLocal = next++; locals.Add(WasmValueType.I64);
        foreach (GuestRegister value in function.Parameters.Concat(function.Locals))
        {
            if (!GuestManagedHeap.ContainsReferences(moduleLayout.Types, value.TypeId)) continue;
            foreach (GuestManagedLeaf leaf in GuestManagedHeap.Leaves(moduleLayout.Types, value.TypeId).Where(item => item.Type.Kind == "managed_ref"))
            {
                managedRoots.Add(new(value.Id, leaf.Offset, moduleLayout.IsMemoryType(value.TypeId), next++));
                locals.Add(WasmValueType.I64);
            }
            GuestType type = moduleLayout.Types[value.TypeId];
            if (type.Kind == GuestBorrowedReference.Kind)
                foreach (GuestManagedLeaf leaf in GuestManagedHeap.Leaves(moduleLayout.Types, type.ElementTypeId!).Where(item => item.Type.Kind == "managed_ref"))
                {
                    managedRoots.Add(new(value.Id, leaf.Offset, true, next++, true)); locals.Add(WasmValueType.I64);
                }
        }
    }

    private void WriteManagedPrologue(WasmBinaryWriter body)
    {
        if (!HasManagedScope) return;
        WasmManagedHeapPlan plan = moduleLayout.ManagedHeap;
        WriteGlobalGet(body, plan.InitializedGlobalIndex); body.WriteByte(0x45);
        body.WriteByte(0x04); body.WriteByte(0x40);
        WriteI32Constant(body, plan.ConfigurationAddress); WriteI32Constant(body, plan.Configuration.Length);
        WriteI32Constant(body, 0); WriteI32Constant(body, 0); ManagedHostCall(body);
        if (plan.StaticConfiguration.Length != 0)
        {
            WriteI32Constant(body, plan.StaticConfigurationAddress); WriteI32Constant(body, plan.StaticConfiguration.Length);
            WriteI32Constant(body, 0); WriteI32Constant(body, 0); ManagedHostCall(body);
        }
        WriteI32Constant(body, 1); WriteGlobalSet(body, plan.InitializedGlobalIndex); body.WriteByte(0x0b);

        // Each activation owns its value copies. Unassigned aggregate reference fields
        // must be null rather than bytes left by an older use of this linear-memory frame.
        foreach (GuestRegister local in function.Locals.Where(value => moduleLayout.IsMemoryType(value.TypeId)
                     && GuestManagedHeap.ContainsReferences(moduleLayout.Types, value.TypeId)))
            WasmMemoryEmitter.WriteZero(body, writer => WriteLocalGet(writer, localIndices[local.Id]), moduleLayout.Types[local.TypeId].Size);

        ManagedHeader(body, GuestManagedHeapCommand.PushFrame); ManagedPacketCall(body, 8, 8); ManagedLoadToken(body, managedFrameLocal);
        foreach (ManagedRoot root in managedRoots)
        {
            ManagedHeader(body, GuestManagedHeapCommand.CreateRoot);
            ManagedStoreToken(body, 8, writer => WriteLocalGet(writer, managedFrameLocal));
            ManagedStoreToken(body, 16, writer => WriteManagedRootValue(writer, root));
            ManagedPacketCall(body, 24, 8); ManagedLoadToken(body, root.LocalIndex);
        }
    }

    private void FlushManagedRoots(WasmBinaryWriter body)
    {
        foreach (ManagedRoot root in managedRoots)
        {
            ManagedHeader(body, GuestManagedHeapCommand.SetRoot);
            ManagedStoreToken(body, 8, writer => WriteLocalGet(writer, root.LocalIndex));
            ManagedStoreToken(body, 16, writer => WriteManagedRootValue(writer, root));
            ManagedPacketCall(body, 24, 0);
        }
    }

    private void WriteManagedRootValue(WasmBinaryWriter body, ManagedRoot root)
    {
        if (root.BorrowedPointee)
        {
            WriteBorrowOwner(body, root.RegisterId); body.WriteByte(0x50); // owner == null
            WriteBorrowI32(body, root.RegisterId, GuestBorrowedReference.AddressOffset); body.WriteByte(0x45); body.WriteByte(0x45);
            body.WriteByte(0x71); body.WriteByte(0x04); body.WriteByte(0x7e); // if (result i64)
            WriteBorrowI32(body, root.RegisterId, GuestBorrowedReference.AddressOffset);
            WriteI32Constant(body, root.Offset); body.WriteByte(0x6a); body.WriteByte(0x29); body.WriteU32(0); body.WriteU32(0);
            body.WriteByte(0x05); body.WriteByte(0x42); body.WriteS64(0); body.WriteByte(0x0b);
            return;
        }
        if (!root.Aggregate && frame.AddressTakenTargets.Contains(root.RegisterId))
        { WriteFrameAddress(body, root.RegisterId); WasmMemoryEmitter.WriteLoad(body, GetValueType(root.RegisterId)); return; }
        WriteLocalGet(body, localIndices[root.RegisterId]);
        if (!root.Aggregate) return;
        WriteI32Constant(body, root.Offset); body.WriteByte(0x6a);
        body.WriteByte(0x29); body.WriteU32(0); body.WriteU32(0);
    }

    private void WriteManagedEpilogue(WasmBinaryWriter body)
    {
        if (!HasManagedScope) return;
        ManagedHeader(body, GuestManagedHeapCommand.PopFrame); ManagedStoreToken(body, 8, writer => WriteLocalGet(writer, managedFrameLocal));
        ManagedPacketCall(body, 16, 0);
    }

    private void CompileManagedInstruction(WasmBinaryWriter body, GuestInstruction instruction)
    {
        if (instruction.Op is GuestStaticStorage.GetOp or GuestStaticStorage.SetOp)
        {
            bool readStatic = instruction.Op == GuestStaticStorage.GetOp;
            var slot = moduleLayout.ManagedHeap.StaticSlots[instruction.TargetId!];
            ManagedHeader(body, readStatic ? GuestManagedHeapCommand.ReadStaticSlot : GuestManagedHeapCommand.WriteStaticSlot);
            ManagedStoreI32(body, 8, slot.Ordinal); ManagedStoreI32(body, 12, slot.TypeOrdinal);
            if (!readStatic) ManagedStoreToken(body, 16, writer => WriteLocalGet(writer, localIndices[instruction.OperandIds[0]]));
            ManagedPacketCall(body, readStatic ? 16 : 24, readStatic ? 8 : 0);
            if (readStatic)
            {
                ManagedLoadToken(body, localIndices[instruction.ResultId!]);
                StoreAddressTakenSlot(body, instruction.ResultId!);
                FlushManagedRoots(body);
            }
            return;
        }
        if (instruction.Op is GuestEventState.SubscribeOp or GuestEventState.ReadOp
            or GuestEventState.LanguageSubscribeOp or GuestEventState.LanguageLookupOp)
        {
            bool subscribe = instruction.Op is GuestEventState.SubscribeOp or GuestEventState.LanguageSubscribeOp;
            bool lookup = instruction.Op == GuestEventState.LanguageLookupOp;
            string state = subscribe ? instruction.OperandIds[3] : instruction.ResultId!;
            if (subscribe || lookup)
                foreach (string operand in instruction.OperandIds.Take(3)) WriteLocalGet(body, localIndices[operand]);
            WriteI32Constant(body, moduleLayout.ManagedHeap.TypeOrdinals[GetValueType(state).Id]);
            if (subscribe) WriteLocalGet(body, localIndices[state]);
            body.WriteByte(0x10); body.WriteU32(moduleLayout.FunctionIndices[instruction.TargetId!]);
            WriteResult(body, instruction);
            // The owner lease spans the return; establish the Guest frame root
            // before any call, allocation or cooperative poll can collect.
            if (!subscribe) FlushManagedRoots(body);
            return;
        }
        if (instruction.Op is GuestContinuationState.StoreOp or GuestContinuationState.ReadOp)
        {
            bool store = instruction.Op == GuestContinuationState.StoreOp;
            string stateReference = store ? instruction.OperandIds[1] : instruction.ResultId!;
            WriteLocalGet(body, localIndices[instruction.OperandIds[0]]);
            WriteI32Constant(body, moduleLayout.ManagedHeap.TypeOrdinals[GetValueType(stateReference).Id]);
            if (store) WriteLocalGet(body, localIndices[stateReference]);
            body.WriteByte(0x10); body.WriteU32(moduleLayout.FunctionIndices[instruction.TargetId!]);
            WriteResult(body, instruction);
            // The continuation lease bridges the host return; establish the frame
            // roots immediately, before any subsequent call, allocation or polling.
            if (!store) FlushManagedRoots(body);
            return;
        }
        if (instruction.Op == "managed_cast")
        {
            string source = instruction.OperandIds[0];
            int expected = moduleLayout.ManagedHeap.TypeOrdinals[GetValueType(instruction.ResultId!).Id];
            // A typed downcast validates owner/generation and the actual object layout.
            // Null propagates without dereferencing; erasure never exposes an address.
            if (expected != 0)
            {
                WriteLocalGet(body, localIndices[source]); body.WriteByte(0x50); body.WriteByte(0x45);
                body.WriteByte(0x04); body.WriteByte(0x40);
                ManagedHeader(body, GuestManagedHeapCommand.ReadBytes);
                ManagedStoreToken(body, 8, writer => WriteLocalGet(writer, localIndices[source]));
                ManagedStoreI32(body, 16, expected); ManagedStoreI32(body, 20, 0); ManagedStoreI32(body, 24, 0);
                ManagedPacketCall(body, 28, 0); body.WriteByte(0x0b);
            }
            WriteLocalGet(body, localIndices[source]); WriteResult(body, instruction); return;
        }
        if (instruction.Op == "managed_collect") { ManagedHeader(body, GuestManagedHeapCommand.Collect); ManagedPacketCall(body, 8, 0); return; }
        if (instruction.Op == "managed_new")
        {
            ManagedRoot root = managedRoots.Single(item => item.RegisterId == instruction.ResultId && !item.Aggregate);
            ManagedHeader(body, GuestManagedHeapCommand.Allocate); ManagedStoreI32(body, 8, moduleLayout.ManagedHeap.TypeOrdinals[GetValueType(instruction.ResultId!).Id]);
            ManagedStoreToken(body, 12, writer => WriteLocalGet(writer, root.LocalIndex));
            ManagedPacketCall(body, 20, 8); ManagedLoadToken(body, localIndices[instruction.ResultId!]); StoreAddressTakenSlot(body, instruction.ResultId!); return;
        }
        string owner = instruction.OperandIds[0];
        GuestType reference = GetValueType(owner);
        GuestField field = moduleLayout.Types[reference.ElementTypeId!].Fields.Single(item => item.Id == instruction.TargetId);
        bool read = instruction.Op == "managed_get";
        string value = read ? instruction.ResultId! : instruction.OperandIds[1];
        GuestType valueType = GetValueType(value);
        bool aggregate = valueType.Storage == "memory";
        if (read && aggregate)
            WasmMemoryEmitter.WriteZero(body, writer => WriteLocalGet(writer, localIndices[value]), valueType.Size);
        IReadOnlyList<GuestManagedLeaf> leaves = GuestManagedHeap.Leaves(moduleLayout.Types, field.TypeId);
        if (leaves.Count == 0)
        {
            ManagedFieldHeader(body, read ? GuestManagedHeapCommand.ReadBytes : GuestManagedHeapCommand.WriteBytes, owner, field.Offset);
            ManagedStoreI32(body, 24, 0); ManagedPacketCall(body, 28, 0); return;
        }
        foreach (GuestManagedLeaf leaf in leaves)
        {
            bool edge = leaf.Type.Kind == "managed_ref";
            GuestManagedHeapCommand command = edge
                ? (read ? GuestManagedHeapCommand.ReadReference : GuestManagedHeapCommand.WriteReference)
                : (read ? GuestManagedHeapCommand.ReadBytes : GuestManagedHeapCommand.WriteBytes);
            ManagedFieldHeader(body, command, owner, checked(field.Offset + leaf.Offset));
            int inputBytes = edge ? 24 : 28;
            if (!edge) ManagedStoreI32(body, 24, leaf.Type.Size);
            if (!read)
            {
                ManagedScratchAddress(body, edge ? 24 : 28);
                WriteLocalGet(body, localIndices[value]);
                if (aggregate)
                {
                    WriteI32Constant(body, leaf.Offset); body.WriteByte(0x6a);
                    WasmMemoryEmitter.WriteLoad(body, leaf.Type);
                }
                WasmMemoryEmitter.WriteStore(body, leaf.Type);
                inputBytes += leaf.Type.Size;
            }
            ManagedPacketCall(body, inputBytes, read ? leaf.Type.Size : 0);
            if (read)
            {
                if (aggregate) { WriteLocalGet(body, localIndices[value]); WriteI32Constant(body, leaf.Offset); body.WriteByte(0x6a); }
                ManagedScratchAddress(body, 48); WasmMemoryEmitter.WriteLoad(body, leaf.Type);
                if (aggregate) WasmMemoryEmitter.WriteStore(body, leaf.Type);
                else { WriteLocalSet(body, localIndices[value]); StoreAddressTakenSlot(body, value); }
            }
        }
    }

    private void ManagedFieldHeader(WasmBinaryWriter body, GuestManagedHeapCommand command, string owner, int offset)
    {
        ManagedHeader(body, command); ManagedStoreToken(body, 8, writer => WriteLocalGet(writer, localIndices[owner]));
        ManagedStoreI32(body, 16, moduleLayout.ManagedHeap.TypeOrdinals[GetValueType(owner).Id]); ManagedStoreI32(body, 20, offset);
    }
    private void ManagedScratchAddress(WasmBinaryWriter body, int offset)
    {
        WriteLocalGet(body, frameBaseLocalIndex); WriteI32Constant(body, checked(frame.ManagedScratchOffset!.Value + offset)); body.WriteByte(0x6a);
    }
    private void ManagedHeader(WasmBinaryWriter body, GuestManagedHeapCommand command)
    {
        ManagedStoreI32(body, 0, unchecked((int)GuestManagedHeap.Magic)); ManagedStoreI32(body, 4, (int)command);
    }
    private void ManagedStoreI32(WasmBinaryWriter body, int offset, int value)
    {
        ManagedScratchAddress(body, offset); WriteI32Constant(body, value); body.WriteByte(0x36); body.WriteU32(0); body.WriteU32(0);
    }
    private void ManagedStoreToken(WasmBinaryWriter body, int offset, Action<WasmBinaryWriter> value)
    {
        ManagedScratchAddress(body, offset); value(body); body.WriteByte(0x37); body.WriteU32(0); body.WriteU32(0);
    }
    private void ManagedLoadToken(WasmBinaryWriter body, uint local)
    {
        ManagedScratchAddress(body, 48); body.WriteByte(0x29); body.WriteU32(0); body.WriteU32(0); WriteLocalSet(body, local);
    }
    private void ManagedPacketCall(WasmBinaryWriter body, int inputBytes, int outputBytes)
    {
        ManagedScratchAddress(body, 0); WriteI32Constant(body, inputBytes);
        if (outputBytes == 0) WriteI32Constant(body, 0); else ManagedScratchAddress(body, 48);
        WriteI32Constant(body, outputBytes); ManagedHostCall(body);
    }
    private void ManagedHostCall(WasmBinaryWriter body)
    {
        body.WriteByte(0x10); body.WriteU32(moduleLayout.FunctionIndices[moduleLayout.ManagedHeap.ImportId!]);
        WriteTrapWhenHostReturnsZero(body);
    }
}
