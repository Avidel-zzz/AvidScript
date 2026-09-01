using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

public static class CSharpGuestDebugMapFinalizer
{
    public static CSharpGuestDebugMap Finalize(
        CSharpGuestDebugMap debugMap,
        GuestWasmDebugOffsetMap offsetMap)
    {
        ArgumentNullException.ThrowIfNull(debugMap);
        ArgumentNullException.ThrowIfNull(offsetMap);

        if (debugMap.SchemaVersion != 2
            || debugMap.DebugVersion != "2.0"
            || offsetMap.SchemaVersion != 1
            || debugMap.ModuleId != offsetMap.ModuleId
            || debugMap.ImportedFunctionCount != offsetMap.ImportedFunctionCount
            || debugMap.DefinedFunctionCount != offsetMap.DefinedFunctionCount
            || debugMap.Provenance.GuestIrSha256 != offsetMap.GuestIrSha256
            || !IsSha256(offsetMap.GuestIrSha256)
            || !IsSha256(offsetMap.WasmSha256))
        {
            throw new InvalidDataException(
                "ASDEBUG2001: Debug map and WASM offset provenance do not match.");
        }

        Dictionary<string, GuestWasmDebugOffset> offsets = new(StringComparer.Ordinal);
        foreach (GuestWasmDebugOffset offset in offsetMap.Offsets)
        {
            if (offset.WasmFunctionIndex < debugMap.ImportedFunctionCount
                || offset.WasmFunctionIndex >= checked(
                    debugMap.ImportedFunctionCount + debugMap.DefinedFunctionCount)
                || offset.FunctionOffset < 0
                || string.IsNullOrWhiteSpace(offset.GuestInstructionId)
                || !offsets.TryAdd(offset.GuestInstructionId, offset))
            {
                throw new InvalidDataException(
                    "ASDEBUG2002: WASM offset artifact contains an invalid or duplicated instruction identity.");
            }
        }

        List<CSharpGuestDebugFunction> functions = new(debugMap.Functions.Count);
        foreach (CSharpGuestDebugFunction function in debugMap.Functions)
        {
            if (function.SequencePoints is null)
            {
                throw new InvalidDataException(
                    $"ASDEBUG2003: Debug function '{function.GuestFunctionId}' has no v2 sequence point array.");
            }

            HashSet<string> operationIds = new(StringComparer.Ordinal);
            List<CSharpGuestDebugSequencePoint> sequencePoints = new(function.SequencePoints.Count);
            foreach (CSharpGuestDebugSequencePoint sequencePoint in function.SequencePoints)
            {
                if (sequencePoint.WasmFunctionOffset != -1
                    || !operationIds.Add(sequencePoint.SemanticOperationId)
                    || !offsets.TryGetValue(
                        sequencePoint.GuestInstructionId,
                        out GuestWasmDebugOffset? offset)
                    || offset.WasmFunctionIndex != function.WasmFunctionIndex)
                {
                    throw new InvalidDataException(
                        $"ASDEBUG2003: Debug function '{function.GuestFunctionId}' has an unresolved sequence point.");
                }
                sequencePoints.Add(sequencePoint with
                {
                    WasmFunctionOffset = offset.FunctionOffset,
                });
            }

            CSharpGuestDebugSequencePoint[] ordered = sequencePoints
                .OrderBy(point => point.WasmFunctionOffset)
                .ThenBy(point => point.GuestInstructionId, StringComparer.Ordinal)
                .ToArray();
            functions.Add(function with { SequencePoints = ordered });
        }

        return debugMap with
        {
            Provenance = debugMap.Provenance with { WasmSha256 = offsetMap.WasmSha256 },
            Functions = functions,
        };
    }

    public static void FinalizeFile(string debugMapPath, string offsetMapPath)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(debugMapPath);
        ArgumentException.ThrowIfNullOrWhiteSpace(offsetMapPath);
        CSharpGuestDebugMap debugMap = CSharpGuestDebugMapSerializer.Deserialize(
            File.ReadAllBytes(debugMapPath));
        GuestWasmDebugOffsetMap offsetMap = GuestWasmDebugOffsetMapSerializer.Deserialize(
            File.ReadAllBytes(offsetMapPath));
        CSharpGuestDebugMapSerializer.Write(debugMapPath, Finalize(debugMap, offsetMap));
    }

    private static bool IsSha256(string value)
    {
        return value.Length == 64 && value.All(character =>
            character is >= '0' and <= '9' or >= 'a' and <= 'f');
    }
}
