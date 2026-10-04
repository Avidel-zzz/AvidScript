using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json.Serialization;

namespace AvidScript.GuestIr;

[JsonUnmappedMemberHandling(JsonUnmappedMemberHandling.Disallow)]
public sealed record GuestTaskValueCatalogPlan(int Version, IReadOnlyList<GuestTaskValueCatalogEntry> Results);
[JsonUnmappedMemberHandling(JsonUnmappedMemberHandling.Disallow)]
public sealed record GuestTaskValueCatalogEntry(int Ordinal, string TypeId, byte[] ValuePlan);

// Optional cold metadata. It declares no executable import or language capability.
public static class GuestTaskValueCatalog
{
    public const uint Magic = 0x31435654, Version = 1;
    public const int MaxCatalogBytes = 2 * 1024 * 1024, MaxResults = 128;
    public const string SectionName = "avidscript.task_values";
    private static readonly UTF8Encoding Utf8 = new(false, true);

    public static GuestTaskValueCatalogPlan Build(IReadOnlyList<GuestType> types, IEnumerable<string> resultTypes)
    {
        ArgumentNullException.ThrowIfNull(resultTypes);
        string[] ids = resultTypes.ToArray();
        if (ids.Length is 0 or > MaxResults || ids.Any(string.IsNullOrWhiteSpace)
            || ids.Distinct(StringComparer.Ordinal).Count() != ids.Length)
            throw new ArgumentException("Task value catalog requires unique bounded result types.");
        return new((int)Version, ids.OrderBy(id => id, StringComparer.Ordinal)
            .Select((id, index) => new GuestTaskValueCatalogEntry(index + 1, id, GuestTaskValuePlan.Build(types, id).Encode())).ToArray());
    }

    // Rebuild from original declarations instead of forwarding caller-owned packets.
    public static byte[] Encode(GuestModule module)
    {
        GuestTaskValueCatalogPlan plan = module.TaskValueCatalog
            ?? throw new ArgumentException("Task value catalog is absent.");
        if (plan.Version != Version || plan.Results is null || plan.Results.Count is 0 or > MaxResults)
            throw new ArgumentException("Task value catalog version or count is invalid.");
        using MemoryStream bytes = new();
        void U32(uint value) { Span<byte> encoded = stackalloc byte[4]; BinaryPrimitives.WriteUInt32LittleEndian(encoded, value); bytes.Write(encoded); }
        void Text(string value, bool optional = false)
        {
            if (value is null || !optional && string.IsNullOrWhiteSpace(value)) throw new ArgumentException("Catalog identity is missing.");
            byte[] encoded = Utf8.GetBytes(value);
            if (encoded.Length > GuestTaskValuePlan.MaxIdentityBytes || value.Any(c => char.IsControl(c)))
                throw new ArgumentException("Catalog identity is invalid.");
            U32((uint)encoded.Length); bytes.Write(encoded);
        }
        void Budget() { if (bytes.Length > MaxCatalogBytes) throw new ArgumentException("Task value catalog exceeds its byte budget."); }
        U32(Magic); U32(Version); Text(module.ModuleId);
        string source = module.Provenance.SourceSha256;
        if (source is null || source.Length != 64 || source.Any(c => !(c is >= '0' and <= '9' or >= 'a' and <= 'f'))
            || source.All(c => c == '0')) throw new ArgumentException("Catalog requires a nonzero canonical source hash.");
        bytes.Write(Convert.FromHexString(source)); U32((uint)plan.Results.Count);
        string? previous = null;
        for (int index = 0; index < plan.Results.Count; ++index)
        {
            GuestTaskValueCatalogEntry entry = plan.Results[index];
            if (entry is null || entry.Ordinal != index + 1 || entry.TypeId is null || entry.ValuePlan is null
                || previous is not null && StringComparer.Ordinal.Compare(previous, entry.TypeId) >= 0)
                throw new ArgumentException("Catalog result identities or ordinals are not canonical.");
            GuestTaskValuePlan value = GuestTaskValuePlan.Build(module.Types, entry.TypeId);
            byte[] packet = value.Encode();
            if (!packet.AsSpan().SequenceEqual(entry.ValuePlan)) throw new ArgumentException("Catalog plan differs from original Guest types.");
            U32((uint)entry.Ordinal); U32((uint)packet.Length); bytes.Write(packet); Budget(); previous = entry.TypeId;
        }
        Dictionary<string, GuestType> types = module.Types.ToDictionary(type => type.Id, StringComparer.Ordinal);
        GuestType[] references = module.Types.Where(type => type.Kind == "managed_ref").OrderBy(type => type.Id, StringComparer.Ordinal).ToArray();
        if (references.Length > GuestManagedHeap.MaxLayouts) throw new ArgumentException("Catalog managed type budget exceeded.");
        Dictionary<string, int> ordinals = GuestManagedHeap.TypeOrdinals(module.Types);
        U32((uint)references.Length); int totalReferences = 0;
        foreach (GuestType reference in references)
        {
            Text(reference.Id); Text(reference.ElementTypeId ?? "", true); U32((uint)ordinals[reference.Id]);
            if (reference.ElementTypeId is null) { U32(0); U32(0); continue; }
            if (!types.TryGetValue(reference.ElementTypeId, out GuestType? payload) || payload.Kind != "struct"
                || payload.Size is <= 0 or > GuestManagedHeap.MaxObjectBytes)
                throw new ArgumentException("Catalog reference requires a bounded struct payload.");
            GuestManagedLeaf[] edges = GuestManagedHeap.Leaves(types, payload.Id).Where(leaf => leaf.Type.Kind == "managed_ref").ToArray();
            totalReferences = checked(totalReferences + edges.Length);
            if (edges.Length > GuestManagedHeap.MaxReferencesPerLayout || totalReferences > GuestManagedHeap.MaxTotalReferences)
                throw new ArgumentException("Catalog payload reference budget exceeded.");
            U32((uint)payload.Size); U32((uint)edges.Length);
            foreach (GuestManagedLeaf edge in edges) { U32((uint)edge.Offset); U32((uint)ordinals[edge.Type.Id]); }
            Budget();
        }
        Budget(); return bytes.ToArray();
    }
}
