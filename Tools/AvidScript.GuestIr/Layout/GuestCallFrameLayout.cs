using System;
using System.Collections.Generic;
using System.Linq;
using System.Security.Cryptography;
using System.Text.Json;

namespace AvidScript.GuestIr;

// Little-endian synchronous frame. All offsets are recomputed from validated types;
// neither a producer-supplied offset nor a pointer to host memory is accepted.
public sealed record GuestCallFrameSlot(int Offset, GuestType Type);
public sealed record GuestCallFrameRoot(int TokenOffset, int ParameterIndex, int ValueOffset);

public sealed record GuestCallFrameLayout(
    IReadOnlyList<GuestCallFrameSlot> Parameters,
    GuestCallFrameSlot Result,
    IReadOnlyList<GuestCallFrameRoot> Roots,
    int ScratchOffset,
    int ByteSize,
    string SignatureSha256)
{
    public const uint Magic = 0x31464341; // ACF1
    public const int Version = 1, HeaderBytes = 64, Alignment = 16;
    public const int MaxBytes = 65536, MaxParameters = 256, MaxExports = 4096;
    public const string SectionName = "avidscript.call_frames";
    public const string HostSectionName = "avidscript.host_call_frames";
    public const int HostSectionVersion = 1;
    public const int HostDispatchSectionVersion = 2;

    public static GuestCallFrameLayout Create(GuestFramedExport export, GuestFunction function,
        IReadOnlyDictionary<string, GuestType> types, IReadOnlyList<GuestFunctionReference> functionReferences)
    {
        if (function.Parameters.Count > MaxParameters || export.ParameterKinds.Count != function.Parameters.Count)
            throw new InvalidOperationException("Framed method parameter count exceeds or disagrees with its contract.");
        int cursor = HeaderBytes;
        GuestCallFrameSlot Slot(string typeId)
        {
            GuestType type = types[typeId];
            if (type.Size < 0 || type.Alignment <= 0 || type.Alignment > Alignment
                || (type.Alignment & (type.Alignment - 1)) != 0)
                throw new InvalidOperationException("Framed method type has an invalid layout.");
            cursor = Align(cursor, type.Alignment);
            GuestCallFrameSlot slot = new(cursor, type);
            cursor = checked(cursor + type.Size);
            if (cursor > MaxBytes) throw new InvalidOperationException("Framed method exceeds its byte limit.");
            return slot;
        }
        GuestCallFrameSlot[] parameters = function.Parameters.Select(parameter => Slot(parameter.TypeId)).ToArray();
        GuestCallFrameSlot result = Slot(function.ReturnTypeId);
        List<GuestCallFrameRoot> roots = new();
        void AddRoots(string typeId, int parameterIndex)
        {
            foreach (GuestManagedLeaf leaf in GuestManagedHeap.Leaves(types, typeId).Where(leaf => leaf.Type.Kind == "managed_ref"))
            {
                cursor = Align(cursor, 8);
                roots.Add(new(cursor, parameterIndex, leaf.Offset));
                cursor = checked(cursor + 8);
                if (cursor > MaxBytes) throw new InvalidOperationException("Framed method root slots exceed its byte limit.");
            }
        }
        AddRoots(result.Type.Id, -1);
        for (int i = 0; i < parameters.Length; ++i)
            if (parameters[i].Type.Kind == GuestBorrowedReference.Kind)
                AddRoots(parameters[i].Type.ElementTypeId!, i);
        int scratch = Align(cursor, Alignment);
        int byteSize = checked(scratch + (roots.Count == 0 ? 0 : 32));
        if (byteSize > MaxBytes) throw new InvalidOperationException("Framed method exceeds its byte limit.");

        // Bind the complete reachable type graph, including nominal identities and
        // recursive managed payloads. Discovery order and unrelated types do not matter.
        HashSet<string> seen = new(StringComparer.Ordinal);
        Stack<string> pending = new(function.Parameters.Select(parameter => parameter.TypeId).Append(function.ReturnTypeId));
        while (pending.TryPop(out string? id))
        {
            if (!seen.Add(id)) continue;
            GuestType type = types[id];
            foreach (GuestField field in type.Fields) pending.Push(field.TypeId);
            if (type.ElementTypeId is not null) pending.Push(type.ElementTypeId);
            if (type.UnderlyingTypeId is not null) pending.Push(type.UnderlyingTypeId);
            if (type.Kind == "function_ref")
            {
                GuestFunctionReference reference = functionReferences.Single(item => item.TypeId == type.Id);
                foreach (string parameter in reference.ParameterTypeIds) pending.Push(parameter);
                pending.Push(reference.ReturnTypeId);
            }
        }
        byte[] canonical = JsonSerializer.SerializeToUtf8Bytes(new
        {
            version = Version,
            parameters = function.Parameters.Select(parameter => parameter.TypeId).ToArray(),
            kinds = export.ParameterKinds,
            result = function.ReturnTypeId,
            types = seen.OrderBy(id => id, StringComparer.Ordinal).Select(id => types[id]).ToArray(),
            function_references = functionReferences.Where(reference => seen.Contains(reference.TypeId))
                .OrderBy(reference => reference.TypeId, StringComparer.Ordinal)
                .Select(reference => new { reference.TypeId, reference.ParameterTypeIds, reference.ReturnTypeId }).ToArray(),
        });
        return new(parameters, result, roots, scratch, byteSize,
            Convert.ToHexString(SHA256.HashData(canonical)).ToLowerInvariant());
    }

    private static int Align(int value, int alignment) => checked((value + alignment - 1) & -alignment);
}
