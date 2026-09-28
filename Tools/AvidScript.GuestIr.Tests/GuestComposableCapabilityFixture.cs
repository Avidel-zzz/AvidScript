using System;
using System.Linq;
using AvidScript.GuestIr;

// Shared by IR validation and emitter admission tests. Both capability
// instruction families occur in the same function of the real IR 35 module.
internal static class GuestComposableCapabilityFixture
{
    internal static GuestModule SynchronousStaticToken()
    {
        GuestModule source = GuestStaticStorageFixture.Create();
        GuestType identity = new("type:int64", "scalar", "i64", Array.Empty<GuestField>(), null, null, 8, 8);
        GuestType[] types = source.Types.Concat(new[] { identity, GuestCancellationTokens.ValueType() }).ToArray();
        GuestLayoutResult layout = GuestLayoutBuilder.Build(types, source.Globals, source.DataSegments);
        if (!layout.Succeeded || layout.Layout is null)
            throw new InvalidOperationException("Combined static/token fixture has an invalid layout.");
        return source with
        {
            SchemaVersion = GuestComposableCapabilities.SchemaVersion,
            IrVersion = GuestComposableCapabilities.IrVersion,
            Language = "csharp",
            Provenance = source.Provenance with { SemanticSchemaVersion = 54, SemanticVersion = "1.63" },
            Types = types,
            MemoryLayout = layout.Layout,
            Functions = source.Functions.Select(function => function.Id == "read" ? function with
            {
                Locals = function.Locals.Concat(new[] { new GuestRegister("identity", identity.Id),
                    new GuestRegister("token", GuestCancellationTokens.TypeId),
                    new GuestRegister("roundtrip", identity.Id) }).ToArray(),
                Blocks = function.Blocks.Select(block => block with
                {
                    Instructions = new[]
                    {
                        new GuestInstruction("constant", "identity", Array.Empty<string>(), null, null, new("int64", "7")),
                        new GuestInstruction("stack_alloc", "token", Array.Empty<string>(), null, null, null),
                        new GuestInstruction("field_store", null, new[] { "token", "identity" }, GuestCancellationTokens.FieldId, null, null),
                        new GuestInstruction("field_load", "roundtrip", new[] { "token" }, GuestCancellationTokens.FieldId, null, null),
                    }.Concat(block.Instructions).ToArray(),
                }).ToArray(),
            } : function).ToArray(),
            CancellationTokens = new(14, "1.13"),
            CapabilityManifest = GuestCapabilityManifest.Create(14, "1.13", new[]
            {
                new GuestCapability(GuestComposableCapabilities.StaticStorage, 1),
                new GuestCapability(GuestComposableCapabilities.CancellationTokenValue, 1),
            }),
        };
    }
}
