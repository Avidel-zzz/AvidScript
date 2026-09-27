using System;
using System.Linq;
using AvidScript.GuestIr;

// Hand-authored IR exercises the value ABI independently of C# lowering.
internal static class GuestCancellationTokenFixture
{
    private const string I32 = "type:int32";
    private const string I64 = "type:int64";
    private const string Token = GuestCancellationTokens.TypeId;
    private const string Root = GuestCancellationTokens.RootTypeId;
    private const string Outcome = "type:outcome_void";

    internal static GuestModule Values()
    {
        GuestType[] types = {
            new("type:void", "void", "none", Array.Empty<GuestField>(), null, null, 0, 1),
            new(I32, "scalar", "i32", Array.Empty<GuestField>(), null, null, 4, 4),
            new(I64, "scalar", "i64", Array.Empty<GuestField>(), null, null, 8, 8),
            new("type:bool", "scalar", "i32", Array.Empty<GuestField>(), null, null, 1, 1),
            GuestCancellationTokens.ValueType(),
        };
        var layout = GuestLayoutBuilder.Build(types, Array.Empty<GuestGlobal>(), Array.Empty<GuestDataSegment>());
        if (!layout.Succeeded || layout.Layout is null) throw new InvalidOperationException("Token fixture layout failed.");
        var wrap = Function("wrap", new[] { Reg("value", I64) }, new[] { Reg("token", Token) }, Token,
            new[] { Op("stack_alloc", "token"), Store("token", GuestCancellationTokens.FieldId, "value") }, "token");
        var copy = Function("copy", new[] { Reg("input", Token) }, Array.Empty<GuestRegister>(), Token,
            Array.Empty<GuestInstruction>(), "input");
        var roundtrip = Function("roundtrip", new[] { Reg("value", I64) },
            new[] { Reg("token", Token), Reg("copied", Token), Reg("identity", I64) }, I64, new[] {
                Op("call", "token", new[] { "value" }, "wrap"),
                Op("call", "copied", new[] { "token" }, "copy"),
                Op("field_load", "identity", new[] { "copied" }, GuestCancellationTokens.FieldId),
            }, "identity");
        var compare = Function("compare", new[] { Reg("left", I64), Reg("right", I64) },
            new[] { Reg("a", Token), Reg("b", Token), Reg("x", I64), Reg("y", I64), Reg("same", "type:bool") },
            "type:bool", new[] {
                Op("call", "a", new[] { "left" }, "wrap"), Op("call", "b", new[] { "right" }, "wrap"),
                Op("field_load", "x", new[] { "a" }, GuestCancellationTokens.FieldId),
                Op("field_load", "y", new[] { "b" }, GuestCancellationTokens.FieldId),
                new GuestInstruction("binary", "same", new[] { "x", "y" }, null, "equals", null),
            }, "same");
        var none = Function("none", Array.Empty<GuestRegister>(),
            new[] { Reg("zero", I64), Reg("token", Token), Reg("identity", I64) }, I64, new[] {
                new GuestInstruction("constant", "zero", Array.Empty<string>(), null, null, new("int64", "0")),
                Op("call", "token", new[] { "zero" }, "wrap"),
                Op("field_load", "identity", new[] { "token" }, GuestCancellationTokens.FieldId),
            }, "identity");
        return new(34, "1.33", "csharp:Scripts/TokenValues.cs", "csharp",
            new("Scripts/TokenValues.cs", new string('a', 64), new string('b', 64), new string('c', 64), 53, "1.62"),
            true, layout.Layout, types, Array.Empty<GuestImport>(), Array.Empty<GuestGlobal>(), layout.DataSegments,
            new[] { wrap, copy, roundtrip, compare, none },
            new[] { new GuestExport("roundtrip", "roundtrip"), new("compare", "compare"), new("none", "none") },
            Array.Empty<GuestDiagnostic>()) { CancellationTokens = new(14, "1.13") };
    }

    internal static GuestModule WithReader()
    {
        var module = Values();
        var payload = new GuestType("type:language_error_payload", "struct", "memory",
            new[] { new GuestField("field:code", "code", I32, 0) }, null, null, 4, 4);
        var root = new GuestType(Root, "managed_ref", "i64", Array.Empty<GuestField>(), payload.Id, null, 8, 8);
        var outcome = new GuestType(Outcome, "struct", "memory", new[] {
            new GuestField("field:status", "status", I32, 0), new("field:error_type", "error_type", I32, 4),
            new("field:source", "source", I32, 8), new("field:error_root", "error_root", Root, 16),
        }, null, null, 24, 8);
        var producer = Function("make_error", Array.Empty<GuestRegister>(),
            new[] { Reg("one", I32), Reg("root", Root), Reg("out", Outcome) }, Outcome, new[] {
                new GuestInstruction("constant", "one", Array.Empty<string>(), null, null, new("int32", "1")),
                Op("managed_new", "root"), Op("managed_set", null, new[] { "root", "one" }, "field:code"),
                Op("stack_alloc", "out"), Store("out", "field:status", "one"),
                Store("out", "field:error_type", "one"), Store("out", "field:source", "one"),
                Store("out", "field:error_root", "root"),
            }, "out");
        var reader = Function("read", new[] { Reg("root", Root) }, new[] { Reg("identity", I64) }, I64,
            new[] { Op("call", "identity", new[] { "root" }, GuestCancellationTokens.ReadImportId) }, "identity");
        return module with {
            CancellationTokens = new(17, "1.16"),
            Types = module.Types.Concat(new[] { payload, root, outcome }).ToArray(),
            Imports = new[] { new GuestImport("import:heap", "avidscript", GuestManagedHeap.ImportName,
                new[] { I32, I32, I32, I32 }, I32), GuestCancellationTokens.Reader() },
            Functions = module.Functions.Concat(new[] { producer, reader }).ToArray(),
            LanguageOutcomeTypes = new[] { new GuestLanguageOutcomeType(Outcome, null) },
            LanguageErrorCatalog = new(new[] { new GuestLanguageErrorTypeToken(1, "type:global::System.Exception") },
                new[] { new GuestLanguageErrorSourceToken(1, "Scripts/TokenValues.cs", 32, 0, 5, 0, 0, 0, 5) }),
        };
    }

    private static GuestRegister Reg(string id, string type) => new(id, type);
    private static GuestInstruction Op(string op, string? result, string[]? operands = null, string? target = null) =>
        new(op, result, operands ?? Array.Empty<string>(), target, null, null);
    private static GuestInstruction Store(string owner, string field, string value) => Op("field_store", null, new[] { owner, value }, field);
    private static GuestFunction Function(string id, GuestRegister[] parameters, GuestRegister[] locals, string result,
        GuestInstruction[] instructions, string value) => new(id, parameters, locals, result, "entry",
            new[] { new GuestBasicBlock("entry", instructions, new("return", null, null, null, value)) });
}
