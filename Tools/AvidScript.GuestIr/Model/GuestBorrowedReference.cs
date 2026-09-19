using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.GuestIr;

public static class GuestBorrowedReference
{
    public const string Kind = "borrowed_ref";
    public const int OwnerOffset = 0, HeapOffset = 8, AddressOffset = 12, Size = 16;

    public static GuestType Declare(string id, string pointee, string erasedOwner, string offsetType) =>
        new(id, Kind, "memory", new[]
        {
            new GuestField("owner", "owner", erasedOwner, OwnerOffset),
            new GuestField("offset", "offset", offsetType, HeapOffset),
            new GuestField("address", "address", offsetType, AddressOffset),
        }, pointee, null, Size, 8);

    public static bool Contains(IReadOnlyDictionary<string, GuestType> types, string id)
    {
        HashSet<string> seen = new(StringComparer.Ordinal); Stack<string> pending = new(); pending.Push(id);
        while (pending.TryPop(out string? current))
        {
            if (!seen.Add(current) || !types.TryGetValue(current, out GuestType? type)) continue;
            if (type.Kind == Kind) return true;
            if (type.Kind == "struct") foreach (GuestField field in type.Fields) pending.Push(field.TypeId);
            if (type.Kind == "array" && type.ElementTypeId is { } element) pending.Push(element);
        }
        return false;
    }
}
