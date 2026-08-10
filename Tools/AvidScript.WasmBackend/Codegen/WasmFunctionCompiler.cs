using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using AvidScript.GuestIr;

namespace AvidScript.WasmBackend;

internal sealed class WasmFunctionCompiler
{
    private readonly GuestModule module;
    private readonly GuestFunction function;
    private readonly WasmModuleLayout moduleLayout;
    private readonly WasmFunctionFrameLayout frame;
    private readonly bool hasArrayAccess;
    private readonly IReadOnlyDictionary<string, GuestRegister> registers;
    private readonly IReadOnlyDictionary<string, GuestGlobal> globals;
    private readonly IReadOnlyDictionary<string, GuestDataSegment> dataSegments;
    private readonly IReadOnlyDictionary<string, GuestFunction> functions;
    private IReadOnlyDictionary<string, uint> localIndices =
        new Dictionary<string, uint>(StringComparer.Ordinal);
    private uint pcLocalIndex;
    private uint frameBaseLocalIndex;
    private uint frameEndLocalIndex;
    private uint arrayAddressLocalIndex;

    public WasmFunctionCompiler(
        GuestModule module,
        GuestFunction function,
        WasmModuleLayout moduleLayout)
    {
        this.module = module;
        this.function = function;
        this.moduleLayout = moduleLayout;
        frame = WasmFunctionFrameLayout.Create(function, moduleLayout);
        hasArrayAccess = function.Blocks
            .SelectMany(block => block.Instructions)
            .Any(instruction => instruction.Op is "array_load" or "array_store");
        registers = function.Parameters
            .Concat(function.Locals)
            .ToDictionary(register => register.Id, StringComparer.Ordinal);
        globals = module.Globals.ToDictionary(item => item.Id, StringComparer.Ordinal);
        dataSegments = module.DataSegments.ToDictionary(item => item.Id, StringComparer.Ordinal);
        functions = module.Functions.ToDictionary(item => item.Id, StringComparer.Ordinal);
    }

    public byte[] Compile()
    {
        WasmBinaryWriter body = new();
        WriteLocals(body);
        WriteFramePrologue(body);
        Dictionary<string, int> blockIndices = function.Blocks
            .Select((block, index) => (block.Id, Index: index))
            .ToDictionary(item => item.Id, item => item.Index, StringComparer.Ordinal);

        body.WriteByte(0x41);
        body.WriteS32(blockIndices[function.EntryBlockId]);
        WriteLocalSet(body, pcLocalIndex);
        body.WriteByte(0x03);
        body.WriteByte(0x40);

        foreach (GuestBasicBlock block in function.Blocks)
        {
            WriteLocalGet(body, pcLocalIndex);
            body.WriteByte(0x41);
            body.WriteS32(blockIndices[block.Id]);
            body.WriteByte(0x46);
            body.WriteByte(0x04);
            body.WriteByte(0x40);

            foreach (GuestInstruction instruction in block.Instructions)
            {
                CompileInstruction(body, instruction);
            }

            CompileTerminator(body, block.Terminator, blockIndices);
            body.WriteByte(0x0b);
        }

        body.WriteByte(0x00);
        body.WriteByte(0x0b);
        body.WriteByte(0x00);
        body.WriteByte(0x0b);
        return body.ToArray();
    }

    private void WriteLocals(WasmBinaryWriter body)
    {
        uint parameterBase = moduleLayout.UsesSRet(function) ? 1u : 0u;
        Dictionary<string, uint> indices = new(StringComparer.Ordinal);
        for (int index = 0; index < function.Parameters.Count; ++index)
        {
            indices.Add(function.Parameters[index].Id, checked(parameterBase + (uint)index));
        }

        List<WasmValueType> locals = new();
        uint firstLocalIndex = checked(parameterBase + (uint)function.Parameters.Count);
        for (int index = 0; index < function.Locals.Count; ++index)
        {
            GuestRegister local = function.Locals[index];
            indices.Add(local.Id, checked(firstLocalIndex + (uint)index));
            locals.Add(moduleLayout.ResolveValueType(local.TypeId));
        }

        pcLocalIndex = checked(firstLocalIndex + (uint)locals.Count);
        locals.Add(WasmValueType.I32);
        if (frame.FrameSize > 0)
        {
            frameBaseLocalIndex = checked(pcLocalIndex + 1);
            frameEndLocalIndex = checked(pcLocalIndex + 2);
            locals.Add(WasmValueType.I32);
            locals.Add(WasmValueType.I32);
        }

        if (hasArrayAccess)
        {
            arrayAddressLocalIndex = checked(
                pcLocalIndex + (frame.FrameSize > 0 ? 3u : 1u));
            locals.Add(WasmValueType.I32);
        }

        localIndices = indices;
        WriteLocalGroups(body, locals);
    }

    private static void WriteLocalGroups(
        WasmBinaryWriter body,
        IReadOnlyList<WasmValueType> locals)
    {
        List<(uint Count, WasmValueType Type)> groups = new();
        foreach (WasmValueType valueType in locals)
        {
            if (groups.Count > 0 && groups[^1].Type == valueType)
            {
                (uint count, WasmValueType type) = groups[^1];
                groups[^1] = (checked(count + 1), type);
            }
            else
            {
                groups.Add((1, valueType));
            }
        }

        body.WriteU32(checked((uint)groups.Count));
        foreach ((uint count, WasmValueType type) in groups)
        {
            body.WriteU32(count);
            body.WriteByte((byte)type);
        }
    }

    private void WriteFramePrologue(WasmBinaryWriter body)
    {
        if (frame.FrameSize == 0)
        {
            return;
        }

        WriteGlobalGet(body, 0);
        WriteLocalSet(body, frameBaseLocalIndex);
        WriteLocalGet(body, frameBaseLocalIndex);
        body.WriteByte(0x41);
        body.WriteS32(frame.FrameSize);
        body.WriteByte(0x6a);
        WriteLocalTee(body, frameEndLocalIndex);
        body.WriteByte(0x3f);
        body.WriteByte(0x00);
        body.WriteByte(0x41);
        body.WriteS32(16);
        body.WriteByte(0x74);
        body.WriteByte(0x4d);
        body.WriteByte(0x04);
        body.WriteByte(0x40);
        body.WriteByte(0x05);
        body.WriteByte(0x00);
        body.WriteByte(0x0b);
        WriteLocalGet(body, frameEndLocalIndex);
        WriteGlobalSet(body, 0);

        foreach (GuestRegister local in function.Locals)
        {
            if (moduleLayout.IsMemoryType(local.TypeId))
            {
                WriteFrameAddress(body, local.Id);
                WriteLocalSet(body, localIndices[local.Id]);
            }
        }

        foreach (GuestRegister parameter in function.Parameters)
        {
            if (frame.HasSlot(parameter.Id))
            {
                WriteFrameAddress(body, parameter.Id);
                WriteLocalGet(body, localIndices[parameter.Id]);
                WasmMemoryEmitter.WriteStore(body, moduleLayout.Types[parameter.TypeId]);
            }
        }
    }

    private void CompileInstruction(
        WasmBinaryWriter body,
        GuestInstruction instruction)
    {
        switch (instruction.Op)
        {
            case "constant":
                CompileConstant(body, instruction);
                break;
            case "copy":
                CompileCopy(body, instruction);
                break;
            case "binary":
                CompileBinary(body, instruction);
                break;
            case "convert":
                CompileConversion(body, instruction);
                break;
            case "call":
                CompileCall(body, instruction);
                break;
            case "local_load":
                CompileLocalLoad(body, instruction);
                break;
            case "local_store":
                CompileLocalStore(body, instruction);
                break;
            case "global_load":
                CompileGlobalLoad(body, instruction);
                break;
            case "global_store":
                CompileGlobalStore(body, instruction);
                break;
            case "stack_alloc":
                RequireMemoryResult(instruction);
                break;
            case "field_load":
                CompileFieldLoad(body, instruction);
                break;
            case "field_store":
                CompileFieldStore(body, instruction);
                break;
            case "address_of":
                CompileAddressOf(body, instruction);
                break;
            case "data_address":
                CompileDataAddress(body, instruction);
                break;
            case "array_load":
                CompileArrayLoad(body, instruction);
                break;
            case "array_length":
                CompileArrayLength(body, instruction);
                break;
            case "array_store":
                CompileArrayStore(body, instruction);
                break;
            case "indirect_load":
                CompileIndirectLoad(body, instruction);
                break;
            case "indirect_store":
                CompileIndirectStore(body, instruction);
                break;
            default:
                throw new NotSupportedException(
                    $"Guest instruction '{instruction.Op}' is not implemented by the WASM backend.");
        }
    }

    private void CompileConstant(WasmBinaryWriter body, GuestInstruction instruction)
    {
        GuestConstant constant = instruction.Constant!;
        GuestType targetType = moduleLayout.Types[registers[instruction.ResultId!].TypeId];
        if (IsMemoryValue(instruction.ResultId!))
        {
            if (constant.Kind != "zero")
            {
                throw new NotSupportedException(
                    $"Memory constant kind '{constant.Kind}' is not implemented.");
            }

            WasmMemoryEmitter.WriteZero(
                body,
                writer => WriteLocalGet(writer, localIndices[instruction.ResultId!]),
                targetType.Size);
            return;
        }

        switch (targetType.Storage)
        {
            case "i32":
                body.WriteByte(0x41);
                body.WriteS32(ParseI32Bits(constant));
                break;
            case "i64":
                body.WriteByte(0x42);
                body.WriteS64(ParseI64Bits(constant));
                break;
            case "f32":
                body.WriteByte(0x43);
                Span<byte> floatBytes = stackalloc byte[4];
                BinaryPrimitives.WriteInt32LittleEndian(
                    floatBytes,
                    BitConverter.SingleToInt32Bits(ParseFloat32(constant)));
                body.WriteBytes(floatBytes);
                break;
            case "f64":
                body.WriteByte(0x44);
                Span<byte> doubleBytes = stackalloc byte[8];
                BinaryPrimitives.WriteInt64LittleEndian(
                    doubleBytes,
                    BitConverter.DoubleToInt64Bits(ParseFloat64(constant)));
                body.WriteBytes(doubleBytes);
                break;
            default:
                throw new NotSupportedException(
                    $"Constant storage '{targetType.Storage}' is not scalar.");
        }

        WriteResult(body, instruction);
    }

    private void CompileCopy(WasmBinaryWriter body, GuestInstruction instruction)
    {
        if (IsMemoryValue(instruction.ResultId!))
        {
            WriteMemoryCopy(
                body,
                instruction.ResultId!,
                instruction.OperandIds[0],
                GetValueType(instruction.ResultId!).Size);
            return;
        }

        WriteLocalGet(body, localIndices[instruction.OperandIds[0]]);
        WriteResult(body, instruction);
    }

    private void CompileBinary(WasmBinaryWriter body, GuestInstruction instruction)
    {
        string leftId = instruction.OperandIds[0];
        string rightId = instruction.OperandIds[1];
        GuestType operandType = GetValueType(leftId);
        WriteLocalGet(body, localIndices[leftId]);
        WriteLocalGet(body, localIndices[rightId]);
        body.WriteByte(ResolveBinaryOpcode(operandType.Storage, instruction.OperatorKind!));
        WriteResult(body, instruction);
    }

    private void CompileConversion(WasmBinaryWriter body, GuestInstruction instruction)
    {
        string operandId = instruction.OperandIds[0];
        GuestType sourceType = GetValueType(operandId);
        GuestType targetType = GetValueType(instruction.ResultId!);
        WriteLocalGet(body, localIndices[operandId]);
        if (!string.Equals(sourceType.Storage, targetType.Storage, StringComparison.Ordinal))
        {
            body.WriteByte(ResolveConversionOpcode(sourceType.Storage, targetType.Storage));
        }

        WriteResult(body, instruction);
    }

    private void CompileCall(WasmBinaryWriter body, GuestInstruction instruction)
    {
        bool usesSRet = functions.TryGetValue(instruction.TargetId!, out GuestFunction? target)
            && moduleLayout.UsesSRet(target);
        if (usesSRet)
        {
            if (instruction.ResultId is null || !IsMemoryValue(instruction.ResultId))
            {
                throw new InvalidOperationException(
                    $"Memory call '{instruction.TargetId}' has no result buffer.");
            }

            WriteLocalGet(body, localIndices[instruction.ResultId]);
        }

        foreach (string operandId in instruction.OperandIds)
        {
            WriteLocalGet(body, localIndices[operandId]);
        }

        body.WriteByte(0x10);
        body.WriteU32(moduleLayout.FunctionIndices[instruction.TargetId!]);
        if (instruction.ResultId is not null && !usesSRet)
        {
            WriteResult(body, instruction);
        }
    }

    private void CompileLocalLoad(WasmBinaryWriter body, GuestInstruction instruction)
    {
        string targetId = instruction.TargetId!;
        GuestType type = GetValueType(targetId);
        if (moduleLayout.IsMemoryType(type.Id))
        {
            WriteMemoryCopy(body, instruction.ResultId!, targetId, type.Size);
        }
        else if (frame.HasSlot(targetId))
        {
            WriteFrameAddress(body, targetId);
            WasmMemoryEmitter.WriteLoad(body, type);
            WriteResult(body, instruction);
        }
        else
        {
            WriteLocalGet(body, localIndices[targetId]);
            WriteResult(body, instruction);
        }
    }

    private void CompileLocalStore(WasmBinaryWriter body, GuestInstruction instruction)
    {
        string targetId = instruction.TargetId!;
        string operandId = instruction.OperandIds[0];
        GuestType type = GetValueType(targetId);
        if (moduleLayout.IsMemoryType(type.Id))
        {
            WriteMemoryCopy(body, targetId, operandId, type.Size);
        }
        else if (frame.HasSlot(targetId))
        {
            WriteFrameAddress(body, targetId);
            WriteLocalGet(body, localIndices[operandId]);
            WasmMemoryEmitter.WriteStore(body, type);
        }
        else
        {
            WriteLocalGet(body, localIndices[operandId]);
            WriteLocalSet(body, localIndices[targetId]);
        }
    }

    private void CompileGlobalLoad(WasmBinaryWriter body, GuestInstruction instruction)
    {
        GuestGlobal global = globals[instruction.TargetId!];
        GuestType type = moduleLayout.Types[global.TypeId];
        GuestStateSlot slot = GetStateSlot(global.Id);
        if (moduleLayout.IsMemoryType(type.Id))
        {
            WriteMemoryCopyFromConstant(
                body,
                instruction.ResultId!,
                slot.Offset,
                type.Size);
        }
        else
        {
            WriteI32Constant(body, slot.Offset);
            WasmMemoryEmitter.WriteLoad(body, type);
            WriteResult(body, instruction);
        }
    }

    private void CompileGlobalStore(WasmBinaryWriter body, GuestInstruction instruction)
    {
        GuestGlobal global = globals[instruction.TargetId!];
        GuestType type = moduleLayout.Types[global.TypeId];
        GuestStateSlot slot = GetStateSlot(global.Id);
        string operandId = instruction.OperandIds[0];
        if (moduleLayout.IsMemoryType(type.Id))
        {
            WriteMemoryCopyToConstant(body, slot.Offset, operandId, type.Size);
        }
        else
        {
            WriteI32Constant(body, slot.Offset);
            WriteLocalGet(body, localIndices[operandId]);
            WasmMemoryEmitter.WriteStore(body, type);
        }
    }

    private void CompileFieldLoad(WasmBinaryWriter body, GuestInstruction instruction)
    {
        string aggregateId = instruction.OperandIds[0];
        GuestField field = ResolveField(GetValueType(aggregateId), instruction.TargetId!);
        GuestType fieldType = moduleLayout.Types[field.TypeId];
        if (moduleLayout.IsMemoryType(fieldType.Id))
        {
            WriteMemoryCopy(
                body,
                instruction.ResultId!,
                aggregateId,
                fieldType.Size,
                0,
                field.Offset);
        }
        else
        {
            WriteLocalGet(body, localIndices[aggregateId]);
            WasmMemoryEmitter.WriteLoad(body, fieldType, checked((uint)field.Offset));
            WriteResult(body, instruction);
        }
    }

    private void CompileFieldStore(WasmBinaryWriter body, GuestInstruction instruction)
    {
        string aggregateId = instruction.OperandIds[0];
        string valueId = instruction.OperandIds[1];
        GuestField field = ResolveField(GetValueType(aggregateId), instruction.TargetId!);
        GuestType fieldType = moduleLayout.Types[field.TypeId];
        if (moduleLayout.IsMemoryType(fieldType.Id))
        {
            WriteMemoryCopy(
                body,
                aggregateId,
                valueId,
                fieldType.Size,
                field.Offset,
                0);
        }
        else
        {
            WriteLocalGet(body, localIndices[aggregateId]);
            WriteLocalGet(body, localIndices[valueId]);
            WasmMemoryEmitter.WriteStore(body, fieldType, checked((uint)field.Offset));
        }
    }

    private void CompileAddressOf(WasmBinaryWriter body, GuestInstruction instruction)
    {
        string targetId = instruction.TargetId!;
        if (moduleLayout.IsMemoryType(GetValueType(targetId).Id))
        {
            WriteLocalGet(body, localIndices[targetId]);
        }
        else
        {
            WriteFrameAddress(body, targetId);
        }

        WriteResult(body, instruction);
    }

    private void CompileDataAddress(WasmBinaryWriter body, GuestInstruction instruction)
    {
        WriteI32Constant(body, dataSegments[instruction.TargetId!].Address);
        WriteResult(body, instruction);
    }

    private void CompileArrayLoad(WasmBinaryWriter body, GuestInstruction instruction)
    {
        if (!moduleLayout.FunctionIndices.ContainsKey(
                GuestArrayCapabilityIntrinsics.LoadImportId))
        {
            CompileLinearArrayLoad(body, instruction);
            return;
        }

        WriteArrayCapabilityCondition(body, instruction.OperandIds[0]);
        body.WriteByte(0x04);
        body.WriteByte(0x40);
        CompileCapabilityArrayLoad(body, instruction);
        body.WriteByte(0x05);
        CompileLinearArrayLoad(body, instruction);
        body.WriteByte(0x0b);
    }

    private void CompileLinearArrayLoad(
        WasmBinaryWriter body,
        GuestInstruction instruction)
    {
        GuestType elementType = moduleLayout.Types[instruction.TargetId!];
        WriteArrayElementAddress(body, instruction);
        if (moduleLayout.IsMemoryType(elementType.Id))
        {
            WasmMemoryEmitter.WriteCopy(
                body,
                writer => WriteLocalGet(writer, localIndices[instruction.ResultId!]),
                writer => WriteLocalGet(writer, arrayAddressLocalIndex),
                elementType.Size);
        }
        else
        {
            WriteLocalGet(body, arrayAddressLocalIndex);
            WasmMemoryEmitter.WriteLoad(body, elementType);
            WriteResult(body, instruction);
        }
    }

    private void CompileCapabilityArrayLoad(
        WasmBinaryWriter body,
        GuestInstruction instruction)
    {
        GuestType elementType = moduleLayout.Types[instruction.TargetId!];
        WriteArrayCapabilityAccessCall(
            body,
            instruction,
            GuestArrayCapabilityIntrinsics.LoadImportId,
            elementType.Size);
        WriteTrapWhenHostReturnsZero(body);
        if (moduleLayout.IsMemoryType(elementType.Id))
        {
            WasmMemoryEmitter.WriteCopy(
                body,
                writer => WriteLocalGet(writer, localIndices[instruction.ResultId!]),
                WriteArrayElementScratchAddress,
                elementType.Size);
        }
        else
        {
            WriteArrayElementScratchAddress(body);
            WasmMemoryEmitter.WriteLoad(body, elementType);
            WriteResult(body, instruction);
        }
    }

    private void CompileArrayLength(WasmBinaryWriter body, GuestInstruction instruction)
    {
        if (!moduleLayout.FunctionIndices.ContainsKey(
                GuestArrayCapabilityIntrinsics.LengthImportId))
        {
            CompileLinearArrayLength(body, instruction);
            return;
        }

        string arrayId = instruction.OperandIds[0];
        WriteArrayCapabilityCondition(body, arrayId);
        body.WriteByte(0x04);
        body.WriteByte(0x40);
        WriteLocalGet(body, localIndices[arrayId]);
        WriteIntrinsicCall(body, GuestArrayCapabilityIntrinsics.LengthImportId);
        WriteResult(body, instruction);
        body.WriteByte(0x05);
        CompileLinearArrayLength(body, instruction);
        body.WriteByte(0x0b);
    }

    private void CompileLinearArrayLength(
        WasmBinaryWriter body,
        GuestInstruction instruction)
    {
        WriteLocalGet(body, localIndices[instruction.OperandIds[0]]);
        WasmMemoryEmitter.WriteLoad(body, GetValueType(instruction.ResultId!));
        WriteResult(body, instruction);
    }

    private void CompileArrayStore(WasmBinaryWriter body, GuestInstruction instruction)
    {
        if (!moduleLayout.FunctionIndices.ContainsKey(
                GuestArrayCapabilityIntrinsics.StoreImportId))
        {
            CompileLinearArrayStore(body, instruction);
            return;
        }

        WriteArrayCapabilityCondition(body, instruction.OperandIds[0]);
        body.WriteByte(0x04);
        body.WriteByte(0x40);
        CompileCapabilityArrayStore(body, instruction);
        body.WriteByte(0x05);
        CompileLinearArrayStore(body, instruction);
        body.WriteByte(0x0b);
    }

    private void CompileLinearArrayStore(
        WasmBinaryWriter body,
        GuestInstruction instruction)
    {
        GuestType elementType = moduleLayout.Types[instruction.TargetId!];
        string valueId = instruction.OperandIds[2];
        WriteArrayElementAddress(body, instruction);
        if (moduleLayout.IsMemoryType(elementType.Id))
        {
            WasmMemoryEmitter.WriteCopy(
                body,
                writer => WriteLocalGet(writer, arrayAddressLocalIndex),
                writer => WriteLocalGet(writer, localIndices[valueId]),
                elementType.Size);
        }
        else
        {
            WriteLocalGet(body, arrayAddressLocalIndex);
            WriteLocalGet(body, localIndices[valueId]);
            WasmMemoryEmitter.WriteStore(body, elementType);
        }
    }

    private void CompileCapabilityArrayStore(
        WasmBinaryWriter body,
        GuestInstruction instruction)
    {
        GuestType elementType = moduleLayout.Types[instruction.TargetId!];
        string valueId = instruction.OperandIds[2];
        if (moduleLayout.IsMemoryType(elementType.Id))
        {
            WasmMemoryEmitter.WriteCopy(
                body,
                WriteArrayElementScratchAddress,
                writer => WriteLocalGet(writer, localIndices[valueId]),
                elementType.Size);
        }
        else
        {
            WriteArrayElementScratchAddress(body);
            WriteLocalGet(body, localIndices[valueId]);
            WasmMemoryEmitter.WriteStore(body, elementType);
        }

        WriteArrayCapabilityAccessCall(
            body,
            instruction,
            GuestArrayCapabilityIntrinsics.StoreImportId,
            elementType.Size);
        WriteTrapWhenHostReturnsZero(body);
    }

    private void WriteArrayCapabilityAccessCall(
        WasmBinaryWriter body,
        GuestInstruction instruction,
        string importId,
        int elementSize)
    {
        WriteLocalGet(body, localIndices[instruction.OperandIds[0]]);
        WriteLocalGet(body, localIndices[instruction.OperandIds[1]]);
        WriteArrayElementScratchAddress(body);
        WriteI32Constant(body, elementSize);
        WriteIntrinsicCall(body, importId);
    }

    private void WriteArrayCapabilityCondition(WasmBinaryWriter body, string arrayId)
    {
        WriteLocalGet(body, localIndices[arrayId]);
        WriteI32Constant(body, 0);
        body.WriteByte(0x48);
    }

    private void WriteArrayElementScratchAddress(WasmBinaryWriter body)
    {
        if (frame.ArrayElementScratchOffset is not int offset)
        {
            throw new InvalidOperationException(
                "Array capability access has no frame scratch storage.");
        }

        WriteLocalGet(body, frameBaseLocalIndex);
        if (offset != 0)
        {
            WriteI32Constant(body, offset);
            body.WriteByte(0x6a);
        }
    }

    private void WriteIntrinsicCall(WasmBinaryWriter body, string importId)
    {
        body.WriteByte(0x10);
        body.WriteU32(moduleLayout.FunctionIndices[importId]);
    }

    private static void WriteTrapWhenHostReturnsZero(WasmBinaryWriter body)
    {
        body.WriteByte(0x45);
        body.WriteByte(0x04);
        body.WriteByte(0x40);
        body.WriteByte(0x00);
        body.WriteByte(0x0b);
    }

    private void WriteArrayElementAddress(
        WasmBinaryWriter body,
        GuestInstruction instruction)
    {
        string arrayId = instruction.OperandIds[0];
        string indexId = instruction.OperandIds[1];
        GuestType arrayType = GetValueType(arrayId);
        GuestType elementType = moduleLayout.Types[instruction.TargetId!];
        WasmMemoryEmitter.WriteArrayElementAddress(
            body,
            writer => WriteLocalGet(writer, localIndices[arrayId]),
            writer => WriteLocalGet(writer, localIndices[indexId]),
            arrayType,
            elementType);
        WriteLocalSet(body, arrayAddressLocalIndex);
    }
    private void CompileIndirectLoad(WasmBinaryWriter body, GuestInstruction instruction)
    {
        GuestType type = moduleLayout.Types[instruction.TargetId!];
        string addressId = instruction.OperandIds[0];
        if (moduleLayout.IsMemoryType(type.Id))
        {
            WriteMemoryCopy(body, instruction.ResultId!, addressId, type.Size);
        }
        else
        {
            WriteLocalGet(body, localIndices[addressId]);
            WasmMemoryEmitter.WriteLoad(body, type);
            WriteResult(body, instruction);
        }
    }

    private void CompileIndirectStore(WasmBinaryWriter body, GuestInstruction instruction)
    {
        GuestType type = moduleLayout.Types[instruction.TargetId!];
        string addressId = instruction.OperandIds[0];
        string valueId = instruction.OperandIds[1];
        if (moduleLayout.IsMemoryType(type.Id))
        {
            WriteMemoryCopy(body, addressId, valueId, type.Size);
        }
        else
        {
            WriteLocalGet(body, localIndices[addressId]);
            WriteLocalGet(body, localIndices[valueId]);
            WasmMemoryEmitter.WriteStore(body, type);
        }
    }

    private void CompileTerminator(
        WasmBinaryWriter body,
        GuestTerminator terminator,
        IReadOnlyDictionary<string, int> blockIndices)
    {
        switch (terminator.Kind)
        {
            case "branch":
                WritePcAndContinue(body, blockIndices[terminator.TargetBlockId!]);
                break;
            case "branch_if":
                WriteLocalGet(body, localIndices[terminator.ConditionValueId!]);
                body.WriteByte(0x04);
                body.WriteByte(0x40);
                WritePc(body, blockIndices[terminator.TargetBlockId!]);
                body.WriteByte(0x05);
                WritePc(body, blockIndices[terminator.FalseTargetBlockId!]);
                body.WriteByte(0x0b);
                body.WriteByte(0x0c);
                body.WriteU32(1);
                break;
            case "return":
                CompileReturn(body, terminator);
                break;
            case "trap":
                body.WriteByte(0x00);
                break;
            default:
                throw new NotSupportedException(
                    $"Guest terminator '{terminator.Kind}' is not implemented by the WASM backend.");
        }
    }

    private void CompileReturn(WasmBinaryWriter body, GuestTerminator terminator)
    {
        if (moduleLayout.UsesSRet(function))
        {
            WasmMemoryEmitter.WriteCopy(
                body,
                writer => WriteLocalGet(writer, 0),
                writer => WriteLocalGet(writer, localIndices[terminator.ReturnValueId!]),
                moduleLayout.Types[function.ReturnTypeId].Size);
        }

        RestoreFrame(body);
        if (!moduleLayout.UsesSRet(function) && terminator.ReturnValueId is not null)
        {
            WriteLocalGet(body, localIndices[terminator.ReturnValueId]);
        }

        body.WriteByte(0x0f);
    }

    private void RestoreFrame(WasmBinaryWriter body)
    {
        if (frame.FrameSize > 0)
        {
            WriteLocalGet(body, frameBaseLocalIndex);
            WriteGlobalSet(body, 0);
        }
    }

    private void WriteMemoryCopy(
        WasmBinaryWriter body,
        string destinationId,
        string sourceId,
        int size,
        int destinationOffset = 0,
        int sourceOffset = 0)
    {
        WasmMemoryEmitter.WriteCopy(
            body,
            writer => WriteLocalGet(writer, localIndices[destinationId]),
            writer => WriteLocalGet(writer, localIndices[sourceId]),
            size,
            destinationOffset,
            sourceOffset);
    }

    private void WriteMemoryCopyFromConstant(
        WasmBinaryWriter body,
        string destinationId,
        int sourceAddress,
        int size)
    {
        WasmMemoryEmitter.WriteCopy(
            body,
            writer => WriteLocalGet(writer, localIndices[destinationId]),
            writer => WriteI32Constant(writer, sourceAddress),
            size);
    }

    private void WriteMemoryCopyToConstant(
        WasmBinaryWriter body,
        int destinationAddress,
        string sourceId,
        int size)
    {
        WasmMemoryEmitter.WriteCopy(
            body,
            writer => WriteI32Constant(writer, destinationAddress),
            writer => WriteLocalGet(writer, localIndices[sourceId]),
            size);
    }

    private void WriteFrameAddress(WasmBinaryWriter body, string valueId)
    {
        if (frame.FrameSize == 0 || !frame.HasSlot(valueId))
        {
            throw new InvalidOperationException(
                $"Value '{valueId}' has no addressable frame slot.");
        }

        WriteLocalGet(body, frameBaseLocalIndex);
        int offset = frame.GetOffset(valueId);
        if (offset != 0)
        {
            WriteI32Constant(body, offset);
            body.WriteByte(0x6a);
        }
    }

    private GuestType GetValueType(string valueId)
    {
        return moduleLayout.Types[registers[valueId].TypeId];
    }

    private bool IsMemoryValue(string valueId)
    {
        return moduleLayout.IsMemoryType(registers[valueId].TypeId);
    }

    private void RequireMemoryResult(GuestInstruction instruction)
    {
        if (instruction.ResultId is null || !IsMemoryValue(instruction.ResultId))
        {
            throw new InvalidOperationException(
                "stack_alloc requires a memory-backed result.");
        }
    }

    private GuestField ResolveField(GuestType aggregateType, string fieldId)
    {
        return aggregateType.Fields.Single(field => string.Equals(
            field.Id,
            fieldId,
            StringComparison.Ordinal));
    }

    private GuestStateSlot GetStateSlot(string globalId)
    {
        return module.MemoryLayout.StateSlots.Single(slot => string.Equals(
            slot.GlobalId,
            globalId,
            StringComparison.Ordinal));
    }

    private void WritePcAndContinue(WasmBinaryWriter body, int blockIndex)
    {
        WritePc(body, blockIndex);
        body.WriteByte(0x0c);
        body.WriteU32(1);
    }

    private void WritePc(WasmBinaryWriter body, int blockIndex)
    {
        WriteI32Constant(body, blockIndex);
        WriteLocalSet(body, pcLocalIndex);
    }

    private void WriteResult(WasmBinaryWriter body, GuestInstruction instruction)
    {
        WriteLocalSet(body, localIndices[instruction.ResultId!]);
    }

    private static void WriteI32Constant(WasmBinaryWriter body, int value)
    {
        body.WriteByte(0x41);
        body.WriteS32(value);
    }

    private static void WriteLocalGet(WasmBinaryWriter body, uint index)
    {
        body.WriteByte(0x20);
        body.WriteU32(index);
    }

    private static void WriteLocalSet(WasmBinaryWriter body, uint index)
    {
        body.WriteByte(0x21);
        body.WriteU32(index);
    }

    private static void WriteLocalTee(WasmBinaryWriter body, uint index)
    {
        body.WriteByte(0x22);
        body.WriteU32(index);
    }

    private static void WriteGlobalGet(WasmBinaryWriter body, uint index)
    {
        body.WriteByte(0x23);
        body.WriteU32(index);
    }

    private static void WriteGlobalSet(WasmBinaryWriter body, uint index)
    {
        body.WriteByte(0x24);
        body.WriteU32(index);
    }

    private static byte ResolveBinaryOpcode(string storage, string operatorKind)
    {
        return (storage, operatorKind) switch
        {
            ("i32", "equals") => 0x46,
            ("i32", "not_equals") => 0x47,
            ("i32", "less_than") => 0x48,
            ("i32", "greater_than") => 0x4a,
            ("i32", "less_than_or_equal") => 0x4c,
            ("i32", "greater_than_or_equal") => 0x4e,
            ("i32", "add") => 0x6a,
            ("i32", "subtract") => 0x6b,
            ("i32", "multiply") => 0x6c,
            ("i32", "divide") => 0x6d,
            ("i32", "remainder") => 0x6f,
            ("i32", "and" or "bitwise_and") => 0x71,
            ("i32", "or" or "bitwise_or") => 0x72,
            ("i32", "exclusive_or" or "bitwise_xor") => 0x73,
            ("i32", "left_shift") => 0x74,
            ("i32", "right_shift") => 0x75,

            ("i64", "equals") => 0x51,
            ("i64", "not_equals") => 0x52,
            ("i64", "less_than") => 0x53,
            ("i64", "greater_than") => 0x55,
            ("i64", "less_than_or_equal") => 0x57,
            ("i64", "greater_than_or_equal") => 0x59,
            ("i64", "add") => 0x7c,
            ("i64", "subtract") => 0x7d,
            ("i64", "multiply") => 0x7e,
            ("i64", "divide") => 0x7f,
            ("i64", "remainder") => 0x81,
            ("i64", "and" or "bitwise_and") => 0x83,
            ("i64", "or" or "bitwise_or") => 0x84,
            ("i64", "exclusive_or" or "bitwise_xor") => 0x85,
            ("i64", "left_shift") => 0x86,
            ("i64", "right_shift") => 0x87,

            ("f32", "equals") => 0x5b,
            ("f32", "not_equals") => 0x5c,
            ("f32", "less_than") => 0x5d,
            ("f32", "greater_than") => 0x5e,
            ("f32", "less_than_or_equal") => 0x5f,
            ("f32", "greater_than_or_equal") => 0x60,
            ("f32", "add") => 0x92,
            ("f32", "subtract") => 0x93,
            ("f32", "multiply") => 0x94,
            ("f32", "divide") => 0x95,

            ("f64", "equals") => 0x61,
            ("f64", "not_equals") => 0x62,
            ("f64", "less_than") => 0x63,
            ("f64", "greater_than") => 0x64,
            ("f64", "less_than_or_equal") => 0x65,
            ("f64", "greater_than_or_equal") => 0x66,
            ("f64", "add") => 0xa0,
            ("f64", "subtract") => 0xa1,
            ("f64", "multiply") => 0xa2,
            ("f64", "divide") => 0xa3,
            _ => throw new NotSupportedException(
                $"Binary operator '{operatorKind}' is not implemented for '{storage}'."),
        };
    }

    private static byte ResolveConversionOpcode(string source, string target)
    {
        return (source, target) switch
        {
            ("i32", "i64") => 0xac,
            ("i32", "f32") => 0xb2,
            ("i32", "f64") => 0xb7,
            ("i64", "i32") => 0xa7,
            ("i64", "f32") => 0xb4,
            ("i64", "f64") => 0xb9,
            ("f32", "i32") => 0xa8,
            ("f32", "i64") => 0xae,
            ("f32", "f64") => 0xbb,
            ("f64", "i32") => 0xaa,
            ("f64", "i64") => 0xb0,
            ("f64", "f32") => 0xb6,
            _ => throw new NotSupportedException(
                $"Conversion from '{source}' to '{target}' is not implemented."),
        };
    }

    private static int ParseI32Bits(GuestConstant constant)
    {
        if (constant.Kind is "zero" or "null")
        {
            return 0;
        }

        if (constant.Kind == "bool")
        {
            return constant.Value == "1" ? 1 : 0;
        }

        if (constant.Kind is "uint8" or "uint16" or "uint32" or "address")
        {
            return unchecked((int)uint.Parse(
                constant.Value!,
                NumberStyles.Integer,
                CultureInfo.InvariantCulture));
        }

        return int.Parse(constant.Value!, NumberStyles.Integer, CultureInfo.InvariantCulture);
    }

    private static long ParseI64Bits(GuestConstant constant)
    {
        if (constant.Kind is "zero" or "null")
        {
            return 0;
        }

        if (constant.Kind == "uint64")
        {
            return unchecked((long)ulong.Parse(
                constant.Value!,
                NumberStyles.Integer,
                CultureInfo.InvariantCulture));
        }

        return long.Parse(constant.Value!, NumberStyles.Integer, CultureInfo.InvariantCulture);
    }

    private static float ParseFloat32(GuestConstant constant)
    {
        if (constant.Kind == "zero")
        {
            return 0;
        }

        return float.Parse(
            constant.Value!,
            NumberStyles.Float,
            CultureInfo.InvariantCulture);
    }

    private static double ParseFloat64(GuestConstant constant)
    {
        if (constant.Kind == "zero")
        {
            return 0;
        }

        return double.Parse(
            constant.Value!,
            NumberStyles.Float,
            CultureInfo.InvariantCulture);
    }
}
