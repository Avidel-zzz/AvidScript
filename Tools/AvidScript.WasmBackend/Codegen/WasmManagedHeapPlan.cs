using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Linq;
using AvidScript.GuestIr;

namespace AvidScript.WasmBackend;

internal sealed class WasmManagedHeapPlan
{
    public Dictionary<string, int> TypeOrdinals { get; } = new(StringComparer.Ordinal);
    public byte[] Configuration { get; private init; } = Array.Empty<byte>();
    public int ConfigurationAddress { get; private init; }
    public byte[] StaticConfiguration { get; private init; } = Array.Empty<byte>();
    public int StaticConfigurationAddress { get; private init; }
    public Dictionary<string, (int Ordinal, int TypeOrdinal)> StaticSlots { get; private init; } = new(StringComparer.Ordinal);
    public int StackStart { get; private init; }
    public uint InitializedGlobalIndex { get; private init; }
    public string? ImportId { get; private init; }
    public bool Enabled => TypeOrdinals.Count != 0;

    public static WasmManagedHeapPlan Create(GuestModule module, IReadOnlyDictionary<string, GuestType> types, bool cooperative)
    {
        GuestType[] references = module.Types.Where(type => type.Kind == "managed_ref" && type.ElementTypeId is not null).OrderBy(type => type.Id, StringComparer.Ordinal).ToArray();
        WasmManagedHeapPlan plan = new() { StackStart = module.MemoryLayout.HeapStart, InitializedGlobalIndex = cooperative ? 2u : 1u };
        if (!module.Types.Any(type => type.Kind == "managed_ref")) return plan;
        for (int index = 0; index < references.Length; ++index) plan.TypeOrdinals.Add(references[index].Id, index + 1);
        foreach (GuestType erased in module.Types.Where(type => type.Kind == "managed_ref" && type.ElementTypeId is null))
            plan.TypeOrdinals.Add(erased.Id, 0);
        List<byte> bytes = new();
        void U32(uint value)
        {
            Span<byte> encoded = stackalloc byte[4]; BinaryPrimitives.WriteUInt32LittleEndian(encoded, value); bytes.AddRange(encoded.ToArray());
        }
        U32(GuestManagedHeap.Magic);
        U32((uint)(references.Length == 0 ? GuestManagedHeapCommand.ConfigureRootsOnly : GuestManagedHeapCommand.Configure));
        if (references.Length != 0) U32((uint)references.Length);
        foreach (GuestType reference in references)
        {
            GuestType payload = types[reference.ElementTypeId!];
            GuestManagedLeaf[] edges = GuestManagedHeap.Leaves(types, payload.Id).Where(leaf => leaf.Type.Kind == "managed_ref").ToArray();
            U32((uint)plan.TypeOrdinals[reference.Id]); U32((uint)payload.Size); U32((uint)edges.Length);
            foreach (GuestManagedLeaf edge in edges) { U32((uint)edge.Offset); U32((uint)plan.TypeOrdinals[edge.Type.Id]); }
        }
        byte[] configuration = bytes.ToArray();
        bytes.Clear();
        if (module.StaticStorage is { } storage)
        {
            U32(GuestManagedHeap.Magic); U32((uint)GuestManagedHeapCommand.ConfigureStaticSlots);
            U32((uint)storage.Slots.Count);
            foreach (GuestStaticSlot slot in storage.Slots)
            {
                int type = plan.TypeOrdinals[slot.TypeId];
                plan.StaticSlots.Add(slot.Id, (plan.StaticSlots.Count + 1, type));
                U32((uint)type);
            }
        }
        int staticAddress = checked(module.MemoryLayout.HeapStart + configuration.Length);
        return new WasmManagedHeapPlan
        {
            Configuration = configuration,
            ConfigurationAddress = module.MemoryLayout.HeapStart,
            StaticConfiguration = bytes.ToArray(),
            StaticConfigurationAddress = staticAddress,
            StaticSlots = plan.StaticSlots,
            StackStart = checked((staticAddress + bytes.Count + 15) & -16),
            InitializedGlobalIndex = plan.InitializedGlobalIndex,
            ImportId = module.Imports.Single(import => import.Module == GuestManagedHeap.ImportModule && import.Name == GuestManagedHeap.ImportName).Id,
            // Dictionary contents are copied below to retain one deterministic owner.
        }.WithOrdinals(plan.TypeOrdinals);
    }
    private WasmManagedHeapPlan WithOrdinals(Dictionary<string, int> ordinals)
    {
        foreach (var pair in ordinals) TypeOrdinals.Add(pair.Key, pair.Value);
        return this;
    }
}
