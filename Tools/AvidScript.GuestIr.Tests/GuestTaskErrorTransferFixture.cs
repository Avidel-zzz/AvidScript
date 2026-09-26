using System;
using System.Linq;
using AvidScript.GuestIr;
using static GuestStaticStorageFixture;

// Generated IR/WASM ownership probe, not a C# source-language acceptance test.
internal static class GuestTaskErrorTransferFixture
{
    internal const string Outcome = "type:transfer_outcome";
    internal const string Root = GuestTaskCancellationFixture.Root;
    internal const string Cache = "static:error_cache";

    internal static GuestModule Create(bool staticStorage = true)
    {
        GuestModule basis = GuestTaskCancellationFixture.Create();
        GuestType outcomeType = new(Outcome, "struct", "memory", new[]
        {
            new GuestField("field:status", "status", I, 0),
            new GuestField("field:error_type", "error_type", I, 4),
            new GuestField("field:source", "source", I, 8),
            new GuestField("field:error_root", "error_root", Root, 16),
            new GuestField("field:value", "value", I, 24),
        }, null, null, 32, 8);
        GuestType[] types = basis.Types.Append(outcomeType).ToArray();
        var layout = GuestLayoutBuilder.Build(types, basis.Globals, basis.DataSegments);
        if (!layout.Succeeded || layout.Layout is null) throw new InvalidOperationException("Transfer fixture layout failed");
        GuestFunction producer = new("sync_error", new[] { Reg("fail", I) },
            new[] { Reg("out", Outcome), Reg("zero", I), Reg("one", I), Reg("answer", I), Reg("root", Root) },
            Outcome, "entry", new[]
            {
                new GuestBasicBlock("entry", new[] { Op("stack_alloc", "out"), Constant("zero", 0),
                    Constant("one", 1), Constant("answer", 41) }, new("branch_if", "fail", "error", "success", null)),
                new GuestBasicBlock("error", new[]
                {
                    staticStorage ? Op(GuestStaticStorage.GetOp, "root", target: Cache) : Op("managed_new", "root"),
                    Op("managed_set", operands: new[] { "root", "one" }, target: "field:code"),
                    Op("managed_collect"),
                    Store("status", "one"), Store("error_type", "one"), Store("source", "one"), Store("error_root", "root"),
                }, Return("out")),
                new GuestBasicBlock("success", new[] { Store("status", "zero"), Store("value", "answer") }, Return("out")),
            });
        GuestFunction transfer = new("transfer", new[] { Reg("task", GuestTaskCancellationFixture.I64), Reg("fail", I) },
            new[] { Reg("outcome", Outcome), Reg("status", I), Reg("type", I), Reg("source", I),
                Reg("root", Root), Reg("accepted", I), Reg("value", I) }, I, "entry", new[]
            {
                new GuestBasicBlock("entry", new[] { Op("call", "outcome", new[] { "fail" }, "sync_error"),
                    Load("status", "status") }, new("branch_if", "status", "error", "success", null)),
                new GuestBasicBlock("error", new[]
                {
                    // Collect after the callee's frame has exited, before extracting its payload.
                    Op("managed_collect"), Load("type", "error_type"), Load("source", "source"), Load("root", "error_root"),
                    Op("call", "accepted", new[] { "task", "type", "source", "root" }, GuestTaskLanguageErrorValidator.ImportId),
                }, Return("accepted")),
                new GuestBasicBlock("success", new[] { Load("value", "value") }, Return("value")),
            });
        GuestFunction init = new("init", Array.Empty<GuestRegister>(),
            new[] { Reg("root", Root), Reg("one", I) }, I, "entry", new[]
            {
                new GuestBasicBlock("entry", staticStorage ? new[]
                {
                    Constant("one", 1), Op("managed_new", "root"),
                    Op("managed_set", operands: new[] { "root", "one" }, target: "field:code"),
                    Op(GuestStaticStorage.SetOp, operands: new[] { "root" }, target: Cache), Op("managed_collect"),
                } : new[] { Constant("one", 1) }, Return("one")),
            });
        GuestFunction clear = new("clear", Array.Empty<GuestRegister>(),
            new[] { Reg("empty", Root), Reg("one", I) }, I, "entry", new[]
            {
                new GuestBasicBlock("entry", staticStorage ? new[]
                {
                    new GuestInstruction("constant", "empty", Array.Empty<string>(), null, null, new("null", null)),
                    Op(GuestStaticStorage.SetOp, operands: new[] { "empty" }, target: Cache),
                    Op("managed_collect"), Constant("one", 1),
                } : new[] { Constant("one", 1) }, Return("one")),
            });
        GuestFunction[] functions = basis.Functions.Concat(new[] { init, clear, producer, transfer }).ToArray();
        const string source = "Scripts/TaskErrorTransfer.cs";
        return basis with
        {
            SchemaVersion = GuestTaskErrorTransfers.SchemaVersion, IrVersion = GuestTaskErrorTransfers.IrVersion,
            ModuleId = "csharp:" + source, Provenance = basis.Provenance with { SourceId = source },
            Types = types, MemoryLayout = layout.Layout, DataSegments = layout.DataSegments, Functions = functions,
            Exports = functions.Where(function => function.Id != producer.Id).Select(function => new GuestExport(function.Id, function.Id)).ToArray(),
            LanguageOutcomeTypes = new[] { new GuestLanguageOutcomeType(Outcome, I) },
            LanguageErrorCatalog = basis.LanguageErrorCatalog! with
            {
                Sources = basis.LanguageErrorCatalog!.Sources.Select(item => item with { SourceId = source }).ToArray(),
            },
            StaticStorage = staticStorage ? new(24, "1.23", new[] { new GuestStaticSlot(Cache, Root) }) : null,
            TaskErrorTransfers = new(24, "1.23", new[] { new GuestTaskErrorTransferSite("transfer", "entry", 0, "error", 4) }),
        };
    }

    private static GuestInstruction Constant(string id, int value) => new("constant", id, Array.Empty<string>(), null, null,
        new("int32", value.ToString(System.Globalization.CultureInfo.InvariantCulture)));
    private static GuestInstruction Store(string field, string value) => Op("field_store", operands: new[] { "out", value }, target: "field:" + field);
    private static GuestInstruction Load(string id, string field) => Op("field_load", id, new[] { "outcome" }, "field:" + field);
    private static GuestTerminator Return(string value) => new("return", null, null, null, value);
}
