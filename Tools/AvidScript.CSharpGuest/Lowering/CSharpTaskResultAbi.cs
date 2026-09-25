using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal sealed record CSharpTaskLanguageError(
    GuestRegister TypeToken,
    GuestRegister SourceToken,
    GuestRegister Root);

internal static class CSharpTaskResultAbi
{
    public const string ImportId = "import:$async:task_i32_v1";
    public const string BindProducerImportId = "import:$async:task_bind_producer_v1";
    public const string PropagateFailureImportId = "import:$async:task_propagate_failure_v1";
    public const string RetainForContinuationImportId = "import:$async:task_retain_for_continuation_v1";
    public const string FaultLanguageErrorImportId = "import:task_fault_language_error_v1";
    public const string LanguageErrorMetaImportId = "import:task_language_error_meta_v1";
    public const string LanguageErrorRootImportId = "import:task_language_error_root_v1";
    public const string LanguageErrorReportImportId = "import:language_error_report_v1";
    public const string TokenTypeId = "type:int64";
    public const string IntTypeId = "type:int32";
    public const int Create = 1;
    public const int Retain = 2;
    public const int Release = 3;
    public const int Await = 4;
    public const int Succeed = 5;
    public const int Read = 7;

    public static string ProducerSlot(SemanticAsyncMethod method) =>
        "$async:producer_task:" + method.MethodSymbolId;

    public static string AwaitSlot(SemanticAsyncAwaitSite site) =>
        "$async:await_task:" + site.CallbackId;

    public static string ExceptionSourceSlot(SemanticAsyncMethod method) =>
        "$async:exception_source:" + method.MethodSymbolId;

    public static string ExceptionTypeSlot(SemanticAsyncMethod method) =>
        "$async:exception_type:" + method.MethodSymbolId;

    public static bool Supports(SemanticDocument document) =>
        (document.SchemaVersion == SemanticContract.TaskResultSchemaVersion
            && document.SemanticVersion == SemanticContract.TaskResultSemanticVersion)
        || (document.SchemaVersion == SemanticContract.TaskLocalSchemaVersion
            && document.SemanticVersion == SemanticContract.TaskLocalSemanticVersion)
        || (document.SchemaVersion == SemanticContract.TaskAssignmentSchemaVersion
            && document.SemanticVersion == SemanticContract.TaskAssignmentSemanticVersion)
        || (document.SchemaVersion == SemanticContract.TaskExistingLocalSchemaVersion
            && document.SemanticVersion == SemanticContract.TaskExistingLocalSemanticVersion)
        || (document.SchemaVersion == SemanticContract.TaskAliasSchemaVersion
            && document.SemanticVersion == SemanticContract.TaskAliasSemanticVersion)
        || (document.SchemaVersion == SemanticContract.AsyncLanguageErrorSchemaVersion
            && document.SemanticVersion == SemanticContract.AsyncLanguageErrorSemanticVersion)
        || (document.SchemaVersion == SemanticContract.AsyncExceptionFlowSchemaVersion
            && document.SemanticVersion == SemanticContract.AsyncExceptionFlowSemanticVersion)
        || (document.SchemaVersion == SemanticContract.DirectAwaitCleanupSchemaVersion
            && document.SemanticVersion == SemanticContract.DirectAwaitCleanupSemanticVersion);

    public static string[] TaskLocalSymbols(SemanticAsyncMethod method) =>
        (method.TaskLocalSymbolIds ?? method.Segments
            .Select(segment => segment.AwaitSite?.TaskLocalSymbolId)
            .OfType<string>().ToArray())
        .OfType<string>().Distinct(StringComparer.Ordinal)
        .OrderBy(symbolId => symbolId, StringComparer.Ordinal).ToArray();

    private static IReadOnlyList<string>? ActiveTaskLocalSymbols(
        SemanticAsyncMethod method, int segmentOrdinal, bool atEntry)
    {
        string[] owned = TaskLocalSymbols(method);
        if (owned.Length == 0) return Array.Empty<string>();
        if (!SemanticAsyncInvocationValidator.TryGetTaskLocalFlow(
                method, owned, method.TaskLocalSymbolIds is not null,
                out _, out _,
                out IReadOnlyDictionary<int, IReadOnlyList<string>> before,
                out IReadOnlyDictionary<int, IReadOnlyList<string>> after))
            return null;
        IReadOnlyDictionary<int, IReadOnlyList<string>> states = atEntry ? before : after;
        return states.TryGetValue(segmentOrdinal, out IReadOnlyList<string>? active)
            ? active : null;
    }

    public static GuestImport Import() => new(ImportId, "avidscript", "avid_task_i32_v1",
        new[] { IntTypeId, TokenTypeId, IntTypeId, IntTypeId }, TokenTypeId);

    public static GuestImport BindProducerImport() => new(BindProducerImportId,
        "avidscript", "avid_task_bind_producer_v1",
        new[] { TokenTypeId, TokenTypeId }, IntTypeId);

    public static GuestImport PropagateFailureImport() => new(PropagateFailureImportId,
        "avidscript", "avid_task_propagate_failure_v1",
        new[] { TokenTypeId, TokenTypeId }, IntTypeId);

    public static GuestImport RetainForContinuationImport() => new(RetainForContinuationImportId,
        "avidscript", "avid_task_retain_for_continuation_v1",
        new[] { TokenTypeId, TokenTypeId }, IntTypeId);

    public static GuestImport FaultLanguageErrorImport() => new(FaultLanguageErrorImportId,
        "avidscript", "avid_task_fault_language_error_v1",
        new[] { TokenTypeId, IntTypeId, IntTypeId, "type:language_error_root" }, IntTypeId);

    public static GuestImport LanguageErrorMetaImport() => new(LanguageErrorMetaImportId,
        "avidscript", "avid_task_language_error_meta_v1",
        new[] { TokenTypeId }, TokenTypeId);

    public static GuestImport LanguageErrorRootImport() => new(LanguageErrorRootImportId,
        "avidscript", "avid_task_language_error_root_v1",
        new[] { TokenTypeId }, "type:language_error_root");

    public static GuestImport LanguageErrorReportImport() => new(LanguageErrorReportImportId,
        "avidscript", "avid_language_error_report_v1",
        new[] { IntTypeId, IntTypeId, "type:language_error_root" }, IntTypeId);

    // The source task must remain owned until both imports have validated its
    // language-error payload and transferred the root into this call frame.
    public static CSharpTaskLanguageError? ReadLanguageError(
        CSharpFunctionLoweringContext context, GuestRegister sourceTask,
        int block, List<GuestInstruction> instructions)
    {
        GuestRegister? metadata = context.CreateTemporary(TokenTypeId, block);
        GuestRegister? shift = Constant(context, TokenTypeId, 32, block, instructions);
        GuestRegister? typePacked = context.CreateTemporary(TokenTypeId, block);
        GuestRegister? typeToken = context.CreateTemporary(IntTypeId, block);
        GuestRegister? sourceToken = context.CreateTemporary(IntTypeId, block);
        GuestRegister? root = context.CreateTemporary("type:language_error_root", block);
        if (metadata is null || shift is null || typePacked is null
            || typeToken is null || sourceToken is null || root is null) return null;
        instructions.Add(new("call", metadata.Id, new[] { sourceTask.Id },
            LanguageErrorMetaImportId, null, null));
        instructions.Add(new("binary", typePacked.Id,
            new[] { metadata.Id, shift.Id }, null, "right_shift", null));
        instructions.Add(new("convert", typeToken.Id,
            new[] { typePacked.Id }, null, null, null));
        instructions.Add(new("convert", sourceToken.Id,
            new[] { metadata.Id }, null, null, null));
        instructions.Add(new("call", root.Id, new[] { sourceTask.Id },
            LanguageErrorRootImportId, null, null));
        return new CSharpTaskLanguageError(typeToken, sourceToken, root);
    }

    public static GuestRegister? LoadTaskLocalToken(CSharpFunctionLoweringContext context,
        string symbolId, int block, List<GuestInstruction> instructions)
    {
        if (!context.TryGetStorage(symbolId, out GuestRegister storage)) return null;
        GuestRegister? value = context.CreateTemporary(storage.TypeId, block);
        GuestRegister? token = context.CreateTemporary(TokenTypeId, block);
        if (value is null || token is null) return null;
        instructions.Add(new("local_load", value.Id, Array.Empty<string>(), storage.Id, null, null));
        instructions.Add(new("convert", token.Id, new[] { value.Id }, null, null, null));
        return token;
    }

    public static bool ReleaseTaskLocal(CSharpFunctionLoweringContext context,
        SemanticAsyncMethod method, int block, List<GuestInstruction> instructions)
    {
        IReadOnlyList<string>? active = ActiveTaskLocalSymbols(method, block, false);
        if (active is null) return false;
        foreach (string symbolId in active)
        {
            GuestRegister? token = LoadTaskLocalToken(context, symbolId, block, instructions);
            if (token is null || Call(context, Release, token, null, block, instructions) is null)
                return false;
        }
        return true;
    }

    public static bool RetainTaskLocal(CSharpFunctionLoweringContext context,
        SemanticAsyncMethod method, int block, List<GuestInstruction> instructions)
    {
        IReadOnlyList<string>? active = ActiveTaskLocalSymbols(method, block, true);
        if (active is null) return false;
        foreach (string symbolId in active)
        {
            GuestRegister? token = LoadTaskLocalToken(context, symbolId, block, instructions);
            if (token is null || Call(context, Retain, token, null, block, instructions) is null)
                return false;
        }
        return true;
    }

    public static bool TransferTaskLocalToContinuation(CSharpFunctionLoweringContext context,
        SemanticAsyncMethod method, GuestRegister continuationToken, int block,
        List<GuestInstruction> instructions)
    {
        IReadOnlyList<string>? active = ActiveTaskLocalSymbols(method, block, false);
        if (active is null) return false;
        foreach (string symbolId in active)
        {
            GuestRegister? token = LoadTaskLocalToken(context, symbolId, block, instructions);
            GuestRegister? accepted = context.CreateTemporary(IntTypeId, block);
            if (token is null || accepted is null) return false;
            instructions.Add(new("call", accepted.Id, new[] { token.Id, continuationToken.Id },
                RetainForContinuationImportId, null, null));
            if (Call(context, Release, token, null, block, instructions) is null) return false;
        }
        return true;
    }

    public static GuestRegister? PropagateFailure(CSharpFunctionLoweringContext context,
        SemanticAsyncMethod method, GuestRegister sourceTask, int block,
        List<GuestInstruction> instructions)
    {
        GuestRegister? targetTask = LoadProducerToken(context, method, block, instructions);
        GuestRegister? accepted = context.CreateTemporary(IntTypeId, block);
        if (targetTask is null || accepted is null) return null;
        instructions.Add(new("call", accepted.Id,
            new[] { sourceTask.Id, targetTask.Id }, PropagateFailureImportId, null, null));
        return accepted;
    }

    public static GuestRegister? BindProducer(CSharpFunctionLoweringContext context,
        SemanticAsyncMethod method, GuestRegister continuationToken, int block,
        List<GuestInstruction> instructions)
    {
        GuestRegister? task = LoadProducerToken(context, method, block, instructions);
        GuestRegister? accepted = context.CreateTemporary(IntTypeId, block);
        if (task is null || accepted is null) return null;
        instructions.Add(new("call", accepted.Id,
            new[] { task.Id, continuationToken.Id }, BindProducerImportId, null, null));
        return accepted;
    }

    public static GuestRegister? Constant(CSharpFunctionLoweringContext context,
        string typeId, long value, int block, List<GuestInstruction> instructions)
    {
        GuestRegister? result = context.CreateTemporary(typeId, block);
        if (result is null) return null;
        instructions.Add(new("constant", result.Id, Array.Empty<string>(), null, null,
            new(typeId == IntTypeId ? "int32" : "int64",
                value.ToString(CultureInfo.InvariantCulture))));
        return result;
    }

    public static GuestRegister? Call(CSharpFunctionLoweringContext context, int command,
        GuestRegister? token, GuestRegister? argument, int block,
        List<GuestInstruction> instructions)
    {
        GuestRegister? commandValue = Constant(context, IntTypeId, command, block, instructions);
        token ??= Constant(context, TokenTypeId, 0, block, instructions);
        argument ??= Constant(context, IntTypeId, 0, block, instructions);
        GuestRegister? reserved = Constant(context, IntTypeId, 0, block, instructions);
        GuestRegister? result = context.CreateTemporary(TokenTypeId, block);
        if (commandValue is null || token is null || argument is null
            || reserved is null || result is null) return null;
        instructions.Add(new("call", result.Id,
            new[] { commandValue.Id, token.Id, argument.Id, reserved.Id },
            ImportId, null, null));
        return result;
    }

    public static GuestRegister? LoadProducerToken(CSharpFunctionLoweringContext context,
        SemanticAsyncMethod method, int block, List<GuestInstruction> instructions)
    {
        if (!context.TryGetStorage(ProducerSlot(method), out GuestRegister storage)) return null;
        GuestRegister? result = context.CreateTemporary(TokenTypeId, block);
        if (result is not null)
            instructions.Add(new("local_load", result.Id, Array.Empty<string>(), storage.Id, null, null));
        return result;
    }

    public static GuestRegister? ReturnValue(CSharpFunctionLoweringContext context,
        SemanticAsyncMethod method, int block, List<GuestInstruction> instructions)
    {
        GuestRegister? token = LoadProducerToken(context, method, block, instructions);
        GuestRegister? result = context.CreateTemporary(context.Callable.ReturnTypeId, block);
        if (token is null || result is null) return null;
        instructions.Add(new("convert", result.Id, new[] { token.Id }, null, null, null));
        return result;
    }
}
