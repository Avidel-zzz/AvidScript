using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal static class CSharpAsyncManagedState
{
    public const string StoreImport = "import:$async:managed_state_store";
    public const string ReadImport = "import:$async:managed_state_read";
    public static string Reference(string payload) => "type:$async:state_ref:" + payload;

    public static void AddTypes(SemanticDocument document, List<GuestType> types)
    {
        // Leave malformed duplicate identities to the canonical layout validator.
        var indexed = types.GroupBy(type => type.Id, StringComparer.Ordinal)
            .ToDictionary(group => group.Key, group => group.First(), StringComparer.Ordinal);
        foreach (SemanticAsyncStateFrame frame in document.AsyncMethods.SelectMany(method => method.Segments)
                     .Select(segment => segment.AwaitSite?.StateFrame).OfType<SemanticAsyncStateFrame>())
            if (GuestManagedHeap.ContainsReferences(indexed, frame.TypeId))
                types.Add(new(Reference(frame.TypeId), "managed_ref", "i64", Array.Empty<GuestField>(), frame.TypeId, null, 8, 8));
    }

    public static GuestImport[] AppendImports(GuestImport[] imports, IEnumerable<GuestFunction> functions)
    {
        string[] operations = functions.SelectMany(fn => fn.Blocks).SelectMany(block => block.Instructions).Select(op => op.Op).ToArray();
        if (operations.Contains(GuestContinuationState.StoreOp))
            imports = imports.Append(new GuestImport(StoreImport, GuestContinuationState.ImportModule, GuestContinuationState.StoreImport,
                new[] { "type:int64", "type:int32", "type:int64" }, "type:int32")).ToArray();
        if (operations.Contains(GuestContinuationState.ReadOp))
            imports = imports.Append(new GuestImport(ReadImport, GuestContinuationState.ImportModule, GuestContinuationState.ReadImport,
                new[] { "type:int64", "type:int32" }, "type:int64")).ToArray();
        return imports;
    }

    public static bool Read(CSharpFunctionLoweringContext context, SemanticAsyncStateFrame frame, GuestType payload,
        int block, GuestRegister token, List<GuestInstruction> instructions,
        out GuestRegister? accepted, out IReadOnlyList<GuestInstruction>? restore)
    {
        accepted = context.CreateTemporary("type:int32", block);
        restore = null;
        GuestRegister? state = context.CreateTemporary(Reference(frame.TypeId), block);
        if (state is null || accepted is null) return false;
        instructions.Add(new(GuestContinuationState.ReadOp, state.Id, new[] { token.Id }, ReadImport, null, null));
        // Invalid reads trap at the typed ABI. Keep the existing resume CFG's
        // accepted branch without introducing a raw memory or nullable-state path.
        instructions.Add(new("constant", accepted.Id, Array.Empty<string>(), null, null, new("int32", "1")));
        List<GuestInstruction> restored = new();
        for (int index = 0; index < frame.Slots.Count; ++index)
        {
            SemanticAsyncStateSlot slot = frame.Slots[index];
            GuestRegister? value = context.CreateTemporary(slot.TypeId, block);
            if (value is null) return false;
            restored.Add(new("managed_get", value.Id, new[] { state.Id }, payload.Fields[index].Id, null, null));
            if (!CSharpOperationLowerer.StoreLocal(context, slot.SymbolId, value, block, restored)) return false;
        }
        restore = restored;
        return true;
    }

    public static bool Store(CSharpFunctionLoweringContext context, SemanticAsyncStateFrame frame, GuestType payload,
        int block, GuestRegister token, List<GuestInstruction> instructions, out GuestRegister? accepted)
    {
        accepted = context.CreateTemporary("type:int32", block);
        GuestRegister? state = context.CreateTemporary(Reference(frame.TypeId), block);
        if (state is null || accepted is null) return false;
        // Each suspension takes new value copies; language reference fields keep
        // their identities and shared object graphs, rather than cloning the graph.
        instructions.Add(new("managed_new", state.Id, Array.Empty<string>(), null, null, null));
        for (int index = 0; index < frame.Slots.Count; ++index)
        {
            SemanticAsyncStateSlot slot = frame.Slots[index];
            if (!context.TryGetStorage(slot.SymbolId, out GuestRegister storage) || storage.TypeId != slot.TypeId)
            { context.Add("ASCG1020", $"Managed async slot '{slot.SymbolId}' has no live storage."); return false; }
            GuestRegister? value = context.CreateTemporary(slot.TypeId, block);
            if (value is null) return false;
            instructions.Add(new("local_load", value.Id, Array.Empty<string>(), storage.Id, null, null));
            instructions.Add(new("managed_set", null, new[] { state.Id, value.Id }, payload.Fields[index].Id, null, null));
        }
        instructions.Add(new(GuestContinuationState.StoreOp, accepted.Id, new[] { token.Id, state.Id }, StoreImport, null, null));
        return true;
    }
}
