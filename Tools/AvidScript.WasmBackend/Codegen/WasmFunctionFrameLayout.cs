using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.GuestIr;

namespace AvidScript.WasmBackend;

internal sealed class WasmFunctionFrameLayout
{
    private readonly IReadOnlyDictionary<string, int> offsets;

    private WasmFunctionFrameLayout(
        IReadOnlyDictionary<string, int> offsets,
        int? arrayElementScratchOffset,
        int frameSize)
    {
        this.offsets = offsets;
        ArrayElementScratchOffset = arrayElementScratchOffset;
        FrameSize = frameSize;
    }

    public int FrameSize { get; }

    public int? ArrayElementScratchOffset { get; }

    public static WasmFunctionFrameLayout Create(
        GuestFunction function,
        WasmModuleLayout moduleLayout)
    {
        HashSet<string> addressTargets = function.Blocks
            .SelectMany(block => block.Instructions)
            .Where(instruction => string.Equals(
                instruction.Op,
                "address_of",
                StringComparison.Ordinal))
            .Select(instruction => instruction.TargetId!)
            .ToHashSet(StringComparer.Ordinal);
        Dictionary<string, int> offsets = new(StringComparer.Ordinal);
        int cursor = 0;

        foreach (GuestRegister parameter in function.Parameters)
        {
            GuestType type = moduleLayout.Types[parameter.TypeId];
            if (!moduleLayout.IsMemoryType(parameter.TypeId)
                && addressTargets.Contains(parameter.Id))
            {
                cursor = AddSlot(offsets, parameter.Id, type, cursor);
            }
        }

        foreach (GuestRegister local in function.Locals)
        {
            GuestType type = moduleLayout.Types[local.TypeId];
            if (moduleLayout.IsMemoryType(local.TypeId)
                || addressTargets.Contains(local.Id))
            {
                cursor = AddSlot(offsets, local.Id, type, cursor);
            }
        }

        bool hasArrayCapabilityAccess = moduleLayout.FunctionIndices.ContainsKey(
                GuestArrayCapabilityIntrinsics.LoadImportId)
            || moduleLayout.FunctionIndices.ContainsKey(
                GuestArrayCapabilityIntrinsics.StoreImportId);
        GuestType[] arrayElementTypes = hasArrayCapabilityAccess
            ? function.Blocks
                .SelectMany(block => block.Instructions)
                .Where(instruction => instruction.Op is "array_load" or "array_store")
                .Select(instruction => moduleLayout.Types[instruction.TargetId!])
                .ToArray()
            : Array.Empty<GuestType>();
        int? arrayElementScratchOffset = null;
        if (arrayElementTypes.Length != 0)
        {
            int scratchAlignment = arrayElementTypes.Max(type => type.Alignment);
            int scratchSize = arrayElementTypes.Max(type => type.Size);
            int offset = AlignUp(cursor, scratchAlignment);
            arrayElementScratchOffset = offset;
            cursor = checked(offset + scratchSize);
        }

        int frameSize = cursor == 0 ? 0 : AlignUp(cursor, 16);
        return new WasmFunctionFrameLayout(offsets, arrayElementScratchOffset, frameSize);
    }

    public bool HasSlot(string valueId)
    {
        return offsets.ContainsKey(valueId);
    }

    public int GetOffset(string valueId)
    {
        return offsets[valueId];
    }

    private static int AddSlot(
        Dictionary<string, int> offsets,
        string valueId,
        GuestType type,
        int cursor)
    {
        int offset = AlignUp(cursor, type.Alignment);
        offsets.Add(valueId, offset);
        return checked(offset + type.Size);
    }

    private static int AlignUp(int value, int alignment)
    {
        if (value < 0
            || alignment <= 0
            || alignment > 16
            || (alignment & (alignment - 1)) != 0)
        {
            throw new OverflowException("Invalid WASM frame size or alignment.");
        }

        return checked((value + alignment - 1) & -alignment);
    }
}
