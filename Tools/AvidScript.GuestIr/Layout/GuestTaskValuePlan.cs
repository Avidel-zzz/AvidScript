using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace AvidScript.GuestIr;

// Kept separate from executable IR capabilities until lowering/import admission
// is connected. Descriptions cannot grant resource authority by themselves.
public enum GuestTaskValueLeafKind
{
    Integer = 1, Float = 2, ManagedReference = 3,
    LinearString = 4, LinearArray = 5, UeHandle = 6,
    ClassReference = 7, FactoryReference = 8, ObjectTypeReference = 9,
    CompositeReference = 10, FunctionReference = 11
}

public sealed record GuestTaskValueLeaf(int Offset, int Size, int Alignment,
    GuestTaskValueLeafKind Kind, string TypeId, string? TargetTypeId);

public sealed class GuestTaskValuePlan
{
    public const uint Magic = 0x31565054, Version = 1;
    public const int MaxValueBytes = 4096, MaxLeaves = 256, MaxIdentityBytes = 1024;
    public const int MaxPlanBytes = 1024 * 1024, ShapeHashBytes = 32;
    private const int MaxTypes = 4096, MaxDepth = 128;
    private static readonly UTF8Encoding Utf8 = new(false, true);
    private readonly byte[] shapeHash;
    public string TypeId { get; }
    public int Size { get; }
    public int Alignment { get; }
    public string ShapeSha256 => Convert.ToHexString(shapeHash).ToLowerInvariant();
    public IReadOnlyList<GuestTaskValueLeaf> Leaves { get; }
    public bool RequiresLease => Leaves.Any(leaf => leaf.Kind is not
        (GuestTaskValueLeafKind.Integer or GuestTaskValueLeafKind.Float));

    private GuestTaskValuePlan(string typeId, int size, int alignment,
        byte[] hash, GuestTaskValueLeaf[] leaves)
    {
        TypeId = typeId; Size = size; Alignment = alignment;
        shapeHash = hash; Leaves = Array.AsReadOnly(leaves);
    }

    public static GuestTaskValuePlan Build(IReadOnlyList<GuestType> types, string typeId)
    {
        if (types.Count == 0 || types.Count > MaxTypes)
            throw new ArgumentException("Task value type table exceeds its bounded contract.");
        Dictionary<string, GuestType> table = new(StringComparer.Ordinal);
        foreach (GuestType type in types)
        {
            CheckIdentity(type.Id);
            if (!table.TryAdd(type.Id, type) || type.Fields.Count > MaxLeaves)
                throw new ArgumentException("Task value has duplicate types or too many fields.");
            HashSet<string> fieldIds = new(StringComparer.Ordinal), names = new(StringComparer.Ordinal);
            foreach (GuestField field in type.Fields)
            {
                CheckIdentity(field.Id); CheckIdentity(field.Name); CheckIdentity(field.TypeId);
                if (!fieldIds.Add(field.Id) || !names.Add(field.Name))
                    throw new ArgumentException("Task value has duplicate field identities.");
            }
        }
        // Guard the existing recursive layout resolver before entering it.
        Dictionary<string, int> depths = new(StringComparer.Ordinal);
        foreach (string id in table.Keys) ValueDepth(id, table, depths, new(StringComparer.Ordinal), 0);
        GuestTypeLayoutResult canonical = GuestDataLayout.ComputeTypes(types);
        if (!canonical.Succeeded || canonical.Types.Any(type => !SameLayout(type, table[type.Id])))
            throw new ArgumentException("Task value requires already validated canonical Guest layouts.");
        if (!table.TryGetValue(typeId, out GuestType? root) || root.Kind == "void"
            || root.Size < 0 || root.Size > MaxValueBytes)
            throw new ArgumentException("Task result has an unknown, void or oversized value type.");

        // Identity covers reachable nominal declarations, including object payloads
        // and array elements. Cyclic object graphs are legal; borrowed aliases are not.
        HashSet<string> reachable = new(StringComparer.Ordinal);
        Stack<string> pendingTypes = new(); pendingTypes.Push(typeId);
        while (pendingTypes.TryPop(out string? current))
        {
            if (!reachable.Add(current)) continue;
            if (!table.TryGetValue(current, out GuestType? type) || type.Kind == "borrowed_ref")
                throw new ArgumentException("Task result contains an unknown type or a borrowed frame alias.");
            foreach (string related in RelatedTypes(type)) pendingTypes.Push(related);
        }
        byte[] hash = SHA256.HashData(JsonSerializer.SerializeToUtf8Bytes(
            reachable.OrderBy(id => id, StringComparer.Ordinal).Select(id => table[id]).ToArray()));
        List<GuestTaskValueLeaf> leaves = new();
        Stack<(string Id, int Offset, int Depth)> pending = new(); pending.Push((typeId, 0, 0));
        while (pending.TryPop(out var entry))
        {
            if (entry.Depth > MaxDepth || leaves.Count + pending.Count > MaxLeaves)
                throw new ArgumentException("Task result exceeds its nesting or leaf limit.");
            GuestType type = table[entry.Id];
            if (type.Kind == "struct")
            {
                foreach (GuestField field in type.Fields.Reverse())
                    pending.Push((field.TypeId, checked(entry.Offset + field.Offset), entry.Depth + 1));
                continue;
            }
            GuestTaskValueLeafKind kind = LeafKind(type);
            string? target = kind is GuestTaskValueLeafKind.ManagedReference or GuestTaskValueLeafKind.LinearArray
                ? type.ElementTypeId : null;
            leaves.Add(new(entry.Offset, type.Size, type.Alignment, kind, type.Id, target));
        }
        return new(typeId, root.Size, root.Alignment, hash, leaves.ToArray());
    }

    // Admission rebuilds from original types. A caller-supplied hash, offset or
    // resource tag is never accepted as a substitute for those declarations.
    public static bool Matches(IReadOnlyList<GuestType> types, string typeId, ReadOnlySpan<byte> packet)
    {
        if (packet.Length > MaxPlanBytes) return false;
        try { return packet.SequenceEqual(Build(types, typeId).Encode()); }
        catch (ArgumentException) { return false; }
        catch (OverflowException) { return false; }
    }

    public byte[] Encode()
    {
        List<byte> bytes = new();
        void U32(int value)
        {
            Span<byte> encoded = stackalloc byte[4];
            BinaryPrimitives.WriteUInt32LittleEndian(encoded, checked((uint)value));
            bytes.AddRange(encoded.ToArray());
        }
        void Identity(string? value)
        {
            byte[] encoded = value is null ? Array.Empty<byte>() : Utf8.GetBytes(value);
            U32(encoded.Length); bytes.AddRange(encoded);
        }
        U32((int)Magic); U32((int)Version); U32(Size); U32(Alignment); U32(Leaves.Count);
        Identity(TypeId); bytes.AddRange(shapeHash);
        foreach (GuestTaskValueLeaf leaf in Leaves)
        {
            U32(leaf.Offset); U32(leaf.Size); U32(leaf.Alignment); U32((int)leaf.Kind);
            Identity(leaf.TypeId); Identity(leaf.TargetTypeId);
        }
        if (bytes.Count > MaxPlanBytes) throw new ArgumentException("Task value plan exceeds its byte limit.");
        return bytes.ToArray();
    }

    private static int ValueDepth(string id, IReadOnlyDictionary<string, GuestType> types,
        Dictionary<string, int> depths, HashSet<string> visiting, int depth)
    {
        if (depth > MaxDepth || !types.TryGetValue(id, out GuestType? type) || !visiting.Add(id))
            throw new ArgumentException("Guest value dependency is missing, recursive or too deep.");
        if (depths.TryGetValue(id, out int existing)) { visiting.Remove(id); return existing; }
        int result = 0;
        IEnumerable<string> children = type.Kind == "struct" || type.Kind == "borrowed_ref"
            ? type.Fields.Select(field => field.TypeId)
            : type.Kind == "enum" && type.UnderlyingTypeId is { } underlying ? new[] { underlying } : Array.Empty<string>();
        foreach (string child in children)
            result = Math.Max(result, checked(1 + ValueDepth(child, types, depths, visiting, depth + 1)));
        visiting.Remove(id);
        if (result > MaxDepth) throw new ArgumentException("Guest value dependency exceeds its depth limit.");
        depths.Add(id, result); return result;
    }

    private static IEnumerable<string> RelatedTypes(GuestType type)
    {
        if (type.ElementTypeId is { } element) yield return element;
        if (type.UnderlyingTypeId is { } underlying) yield return underlying;
        foreach (GuestField field in type.Fields) yield return field.TypeId;
    }

    private static bool SameLayout(GuestType left, GuestType right) => left.Id == right.Id
        && left.Kind == right.Kind && left.Storage == right.Storage && left.Size == right.Size
        && left.Alignment == right.Alignment && left.ElementTypeId == right.ElementTypeId
        && left.UnderlyingTypeId == right.UnderlyingTypeId && left.Fields.SequenceEqual(right.Fields);

    private static GuestTaskValueLeafKind LeafKind(GuestType type) => type.Kind switch
    {
        "scalar" or "enum" when type.Storage is "i32" or "i64" => GuestTaskValueLeafKind.Integer,
        "scalar" when type.Storage is "f32" or "f64" => GuestTaskValueLeafKind.Float,
        "managed_ref" => GuestTaskValueLeafKind.ManagedReference,
        "string" => GuestTaskValueLeafKind.LinearString,
        "array" => GuestTaskValueLeafKind.LinearArray,
        "handle" => GuestTaskValueLeafKind.UeHandle,
        "class_ref" => GuestTaskValueLeafKind.ClassReference,
        "factory_ref" => GuestTaskValueLeafKind.FactoryReference,
        "object_type_ref" => GuestTaskValueLeafKind.ObjectTypeReference,
        "composite_ref" => GuestTaskValueLeafKind.CompositeReference,
        "function_ref" => GuestTaskValueLeafKind.FunctionReference,
        _ => throw new ArgumentException($"Guest type '{type.Id}' cannot be a persistent task value.")
    };

    private static void CheckIdentity(string value)
    {
        if (string.IsNullOrWhiteSpace(value) || value.Any(char.IsControl)
            || Utf8.GetByteCount(value) > MaxIdentityBytes)
            throw new ArgumentException("Task value identity is empty, invalid UTF-8 or too long.");
    }
}
