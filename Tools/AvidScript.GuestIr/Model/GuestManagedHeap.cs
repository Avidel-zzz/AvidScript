using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.GuestIr;

// Mirrored wire constants are checked against AvidScriptManagedHeapAbi.h by the backend tests.
public static class GuestManagedHeap
{
    public const string ImportModule = "avidscript";
    public const string ImportName = "avid_managed_heap_v1";
    public const uint Magic = 0x3150484d;
    public const int MaxLayouts = 1024, MaxReferencesPerLayout = 256, MaxTotalReferences = 65536;
    public const int MaxObjectBytes = 65536;

    public static bool ContainsReferences(IReadOnlyDictionary<string, GuestType> types, string id)
    {
        Stack<string> pending = new(); HashSet<string> seen = new(StringComparer.Ordinal); pending.Push(id);
        while (pending.TryPop(out string? current))
        {
            if (!seen.Add(current) || !types.TryGetValue(current, out GuestType? type)) continue;
            if (type.Kind == "managed_ref") return true;
            if (type.Kind is "struct" or "borrowed_ref") foreach (GuestField field in type.Fields) pending.Push(field.TypeId);
        }
        return false;
    }

    public static IReadOnlyList<GuestManagedLeaf> Leaves(IReadOnlyDictionary<string, GuestType> types, string id)
    {
        List<GuestManagedLeaf> result = new();
        Stack<(string Id, int Offset, int Depth)> pending = new(); pending.Push((id, 0, 0));
        while (pending.TryPop(out var entry))
        {
            if (entry.Depth > 128 || result.Count + pending.Count > MaxObjectBytes)
                throw new InvalidOperationException("Managed aggregate nesting or leaf count exceeds its bounded layout contract.");
            GuestType type = types[entry.Id];
            if (type.Kind is "struct" or "borrowed_ref")
            {
                foreach (GuestField field in type.Fields.Reverse())
                    pending.Push((field.TypeId, checked(entry.Offset + field.Offset), entry.Depth + 1));
            }
            else result.Add(new(entry.Offset, type));
        }
        return result;
    }

    public static bool NeedsScope(GuestFunction function, IReadOnlyDictionary<string, GuestType> types) =>
        function.Parameters.Concat(function.Locals).Any(value => ContainsReferences(types, value.TypeId))
        || function.Blocks.SelectMany(block => block.Instructions).Any(instruction => instruction.Op.StartsWith("managed_", StringComparison.Ordinal));
}

public sealed record GuestManagedLeaf(int Offset, GuestType Type);

public enum GuestManagedHeapCommand
{
    Configure = 1, PushFrame = 2, PopFrame = 3, CreateRoot = 4,
    SetRoot = 5, ReleaseRoot = 6, Allocate = 7, ReadBytes = 8,
    WriteBytes = 9, ReadReference = 10, WriteReference = 11, Collect = 12,
    ConfigureRootsOnly = 13,
}
