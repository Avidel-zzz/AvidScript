using System;
using System.Linq;
using AvidScript.GuestIr;

// Shared by the IR and production WASM backend tests. This is an ABI fixture,
// not evidence that the C# cancellation control-flow lowerer is implemented.
internal static class GuestTaskCancellationFixture
{
    internal const string TaskCanceledType = "type:global::System.Threading.Tasks.TaskCanceledException";
    internal const string OperationCanceledType = "type:global::System.OperationCanceledException";
    internal const string Root = "type:language_error_root";
    internal const string I32 = "type:int32";
    internal const string I64 = "type:int64";

    internal static GuestModule Create()
    {
        GuestType[] types =
        {
            new("type:void", "void", "none", Array.Empty<GuestField>(), null, null, 0, 1),
            new(I32, "scalar", "i32", Array.Empty<GuestField>(), null, null, 4, 4),
            new(I64, "scalar", "i64", Array.Empty<GuestField>(), null, null, 8, 8),
            new("type:bool", "scalar", "i32", Array.Empty<GuestField>(), null, null, 1, 1),
            new("type:language_error_payload", "struct", "memory",
                new[] { new GuestField("field:code", "code", I32, 0) }, null, null, 4, 4),
            new(Root, "managed_ref", "i64", Array.Empty<GuestField>(),
                "type:language_error_payload", null, 8, 8),
        };
        GuestLayoutResult layout = GuestLayoutBuilder.Build(types,
            Array.Empty<GuestGlobal>(), Array.Empty<GuestDataSegment>());
        if (!layout.Succeeded || layout.Layout is null)
            throw new InvalidOperationException("Cancellation fixture layout failed.");
        GuestImport[] imports =
        {
            new("import:heap", "avidscript", GuestManagedHeap.ImportName,
                new[] { I32, I32, I32, I32 }, I32),
            new("import:task", "avidscript", "avid_task_i32_v1",
                new[] { I32, I64, I32, I32 }, I64),
            new("import:bind", "avidscript", "avid_task_bind_producer_v1", new[] { I64, I64 }, I32),
            new("import:propagate", "avidscript", "avid_task_propagate_failure_v1", new[] { I64, I64 }, I32),
            new("import:retain", "avidscript", "avid_task_retain_for_continuation_v1", new[] { I64, I64 }, I32),
            new(GuestTaskLanguageErrorValidator.ImportId, "avidscript",
                GuestTaskLanguageErrorValidator.ImportName, new[] { I64, I32, I32, Root }, I32),
            new(GuestTaskCancellationErrorValidator.ImportId, "avidscript",
                GuestTaskCancellationErrorValidator.ImportName, new[] { I64, I32, I32, Root }, I32),
            new(GuestTaskCancellationErrorValidator.MetaImportId, "avidscript",
                GuestTaskCancellationErrorValidator.MetaImportName, new[] { I64 }, I64),
            new(GuestTaskCancellationErrorValidator.RootImportId, "avidscript",
                GuestTaskCancellationErrorValidator.RootImportName, new[] { I64 }, Root),
        };
        GuestFunction reader = new("read", new[] { new GuestRegister("task", I64) },
            new[]
            {
                new GuestRegister("metadata", I64), new GuestRegister("first", Root),
                new GuestRegister("second", Root), new GuestRegister("same", "type:bool"),
            }, I64, "entry", new[]
            {
                new GuestBasicBlock("entry", new GuestInstruction[]
                {
                    new("call", "metadata", new[] { "task" }, GuestTaskCancellationErrorValidator.MetaImportId, null, null),
                    new("call", "first", new[] { "task" }, GuestTaskCancellationErrorValidator.RootImportId, null, null),
                    new("call", "second", new[] { "task" }, GuestTaskCancellationErrorValidator.RootImportId, null, null),
                    new("binary", "same", new[] { "first", "second" }, null, "equals", null),
                }, new("branch_if", "same", "accepted", "rejected", null)),
                new GuestBasicBlock("accepted", Array.Empty<GuestInstruction>(), new("return", null, null, null, "metadata")),
                new GuestBasicBlock("rejected", Array.Empty<GuestInstruction>(), new("trap", null, null, null, null)),
            });
        GuestFunction[] functions =
        {
            Producer("cancel", GuestTaskCancellationErrorValidator.ImportId),
            Producer("fault", GuestTaskLanguageErrorValidator.ImportId), reader,
        };
        return new GuestModule(24, "1.23", "csharp:Scripts/Cancellation.cs", "csharp",
            new GuestProvenance("Scripts/Cancellation.cs", new string('a', 64),
                new string('b', 64), new string('c', 64), 44, "1.53"), true,
            layout.Layout, types, imports, Array.Empty<GuestGlobal>(), layout.DataSegments,
            functions, functions.Select(function => new GuestExport(function.Id, function.Id)).ToArray(),
            Array.Empty<GuestDiagnostic>())
        {
            LanguageErrorCatalog = new GuestLanguageErrorCatalog(
                new[] { new GuestLanguageErrorTypeToken(1, TaskCanceledType) },
                new[] { new GuestLanguageErrorSourceToken(1, "Scripts/Cancellation.cs",
                    32, 0, 5, 0, 0, 0, 5) }),
        };
    }

    private static GuestFunction Producer(string id, string import) => new(id,
        new[] { new GuestRegister("task", I64) }, new[]
        {
            new GuestRegister("type", I32), new GuestRegister("source", I32),
            new GuestRegister("root", Root), new GuestRegister("accepted", I32),
        }, I32, "entry", new[]
        {
            new GuestBasicBlock("entry", new GuestInstruction[]
            {
                new("constant", "type", Array.Empty<string>(), null, null, new("int32", "1")),
                new("constant", "source", Array.Empty<string>(), null, null, new("int32", "1")),
                new("managed_new", "root", Array.Empty<string>(), null, null, null),
                new("managed_set", null, new[] { "root", "type" }, "field:code", null, null),
                new("call", "accepted", new[] { "task", "type", "source", "root" }, import, null, null),
            }, new("return", null, null, null, "accepted")),
        });
}
