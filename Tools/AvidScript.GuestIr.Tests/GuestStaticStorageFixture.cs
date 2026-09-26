using System;
using System.Linq;
using AvidScript.GuestIr;

// Shared IR/backend fixture. Source-language static initialization is tested
// separately when the C# frontend emits this storage contract.
internal static class GuestStaticStorageFixture
{
    internal const string I = "type:int32", V = "type:void", B = "type:bool", F = "type:float32";
    internal const string R = "type:node_ref", E = "type:erased", Node = "type:node";
    internal const string Current = "static:current", Alias = "static:alias", Erased = "static:erased";

    internal static GuestModule Create()
    {
        GuestType[] types =
        {
            new(V, "void", "none", Array.Empty<GuestField>(), null, null, 0, 1),
            new(I, "scalar", "i32", Array.Empty<GuestField>(), null, null, 4, 4),
            new(B, "scalar", "i32", Array.Empty<GuestField>(), null, null, 1, 1),
            new(F, "scalar", "f32", Array.Empty<GuestField>(), null, null, 4, 4),
            new(Node, "struct", "memory", new[] { new GuestField("next", "Next", R, 0),
                new GuestField("number", "Number", I, 8) }, null, null, 16, 8),
            new(R, "managed_ref", "i64", Array.Empty<GuestField>(), Node, null, 8, 8),
            new(E, "managed_ref", "i64", Array.Empty<GuestField>(), null, null, 8, 8),
        };
        GuestLayoutResult layout = GuestLayoutBuilder.Build(types, Array.Empty<GuestGlobal>(), Array.Empty<GuestDataSegment>());
        if (!layout.Succeeded || layout.Layout is null) throw new InvalidOperationException("Static fixture layout failed.");
        GuestFunction tick = Function("tick", new[] { Reg("dt", F) },
            new[] { Reg("a", R), Reg("b", R), Reg("erased", E), Reg("x", I), Reg("y", I) }, V,
            new[]
            {
                Op("managed_new", "a"), Op("managed_new", "b"), Constant("x", 40), Constant("y", 2),
                Op("managed_set", null, new[] { "a", "x" }, "number"), Op("managed_set", null, new[] { "b", "y" }, "number"),
                Op("managed_set", null, new[] { "a", "b" }, "next"), Op("managed_set", null, new[] { "b", "a" }, "next"),
                Op(GuestStaticStorage.SetOp, null, new[] { "a" }, Current),
                Op(GuestStaticStorage.SetOp, null, new[] { "a" }, Alias),
                Op("managed_cast", "erased", new[] { "a" }), Op(GuestStaticStorage.SetOp, null, new[] { "erased" }, Erased),
            });
        GuestInstruction[] clear =
        {
            Null("empty"), Null("emptyErased"),
            Op(GuestStaticStorage.SetOp, null, new[] { "empty" }, Current),
            Op(GuestStaticStorage.SetOp, null, new[] { "empty" }, Alias),
            Op(GuestStaticStorage.SetOp, null, new[] { "emptyErased" }, Erased),
        };
        GuestInstruction[] read =
        {
            Op("managed_get", "next", new[] { "a" }, "next"),
            Op("managed_get", "x", new[] { "a" }, "number"), Op("managed_get", "y", new[] { "next" }, "number"),
            new GuestInstruction("binary", "sum", new[] { "x", "y" }, null, "add", null),
        };
        GuestRegister[] readLocals = { Reg("a", R), Reg("next", R), Reg("x", I), Reg("y", I), Reg("sum", I) };
        GuestFunction[] functions =
        {
            Function("begin", Array.Empty<GuestRegister>(), Array.Empty<GuestRegister>(), V, Array.Empty<GuestInstruction>()),
            tick,
            Function("read", new[] { Reg("unused1", I), Reg("unused2", I) }, readLocals, I,
                new[] { Op(GuestStaticStorage.GetOp, "a", target: Current) }.Concat(read).ToArray(), "sum"),
            Function("hold", Array.Empty<GuestRegister>(), readLocals.Concat(new[] { Reg("empty", R), Reg("emptyErased", E) }).ToArray(), I,
                new[] { Op(GuestStaticStorage.GetOp, "a", target: Current) }.Concat(clear)
                    .Append(Op("managed_collect")).Concat(read).ToArray(), "sum"),
            Function("clear", Array.Empty<GuestRegister>(), new[] { Reg("empty", R), Reg("emptyErased", E) }, V, clear),
            Function("default", Array.Empty<GuestRegister>(), new[] { Reg("a", R), Reg("empty", R), Reg("isNull", B) }, B,
                new[] { Op(GuestStaticStorage.GetOp, "a", target: Current), Null("empty"),
                    new GuestInstruction("binary", "isNull", new[] { "a", "empty" }, null, "equals", null) }, "isNull"),
        };
        string hash = new('a', 64);
        return new GuestModule(GuestStaticStorage.SchemaVersion, GuestStaticStorage.IrVersion,
            "managed-static", "guest-ir", new("Fixtures/StaticStorage", hash, hash, hash, 4, "1.4"), true,
            layout.Layout, types, new[] { new GuestImport("import:heap", GuestManagedHeap.ImportModule,
                GuestManagedHeap.ImportName, new[] { I, I, I, I }, I) }, Array.Empty<GuestGlobal>(), layout.DataSegments,
            functions, new[] { new GuestExport("avid_on_begin_play", "begin"), new GuestExport("avid_on_tick", "tick"),
                new GuestExport("static_read", "read"), new GuestExport("static_hold", "hold"),
                new GuestExport("static_clear", "clear"), new GuestExport("static_default", "default") }, Array.Empty<GuestDiagnostic>())
        {
            StaticStorage = new(14, "1.13", new[] { new GuestStaticSlot(Current, R), new GuestStaticSlot(Alias, R), new GuestStaticSlot(Erased, E) }),
        };
    }

    internal static GuestModule WithCancellation()
    {
        GuestModule basis = GuestTaskCancellationFixture.Create();
        return basis with
        {
            SchemaVersion = GuestStaticStorage.SchemaVersion, IrVersion = GuestStaticStorage.IrVersion,
            StaticStorage = new(basis.SchemaVersion, basis.IrVersion, new[] { new GuestStaticSlot("static:cache", GuestTaskCancellationFixture.Root) }),
            Functions = basis.Functions.Select(function => function.Id is "cancel" or "fault" ? function with
            {
                Locals = function.Locals.Append(Reg("cache", GuestTaskCancellationFixture.Root)).ToArray(),
                Blocks = function.Blocks.Select(block => block with
                {
                    // The submitted Task error must remain fresh. Keep a separate
                    // object in static storage before building the error payload.
                    Instructions = new[] { Op("managed_new", "cache"),
                        Op(GuestStaticStorage.SetOp, null, new[] { "cache" }, "static:cache") }
                        .Concat(block.Instructions).ToArray(),
                }).ToArray(),
            } : function).ToArray(),
        };
    }

    internal static GuestRegister Reg(string id, string type) => new(id, type);
    internal static GuestInstruction Op(string op, string? result = null, string[]? operands = null, string? target = null) =>
        new(op, result, operands ?? Array.Empty<string>(), target, null, null);
    private static GuestInstruction Constant(string id, int value) => new("constant", id, Array.Empty<string>(), null, null, new("int32", value.ToString(System.Globalization.CultureInfo.InvariantCulture)));
    private static GuestInstruction Null(string id) => new("constant", id, Array.Empty<string>(), null, null, new("null", null));
    private static GuestFunction Function(string id, GuestRegister[] parameters, GuestRegister[] locals, string result,
        GuestInstruction[] instructions, string? returned = null) => new(id, parameters, locals, result, "entry",
            new[] { new GuestBasicBlock("entry", instructions, new("return", null, null, null, returned)) });
}
