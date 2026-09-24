using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal static class CSharpTaskResultAbi
{
    public const string ImportId = "import:$async:task_i32_v1";
    public const string BindProducerImportId = "import:$async:task_bind_producer_v1";
    public const string PropagateFailureImportId = "import:$async:task_propagate_failure_v1";
    public const string RetainForContinuationImportId = "import:$async:task_retain_for_continuation_v1";
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

    public static bool Supports(SemanticDocument document) =>
        (document.SchemaVersion == SemanticContract.TaskResultSchemaVersion
            && document.SemanticVersion == SemanticContract.TaskResultSemanticVersion)
        || (document.SchemaVersion == SemanticContract.TaskLocalSchemaVersion
            && document.SemanticVersion == SemanticContract.TaskLocalSemanticVersion);

    public static string? TaskLocalSymbol(SemanticAsyncMethod method) => method.Segments
        .Select(segment => segment.AwaitSite?.TaskLocalSymbolId)
        .FirstOrDefault(symbolId => symbolId is not null);

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

    public static GuestRegister? LoadTaskLocalToken(CSharpFunctionLoweringContext context,
        SemanticAsyncMethod method, int block, List<GuestInstruction> instructions)
    {
        if (!context.TryGetStorage(TaskLocalSymbol(method), out GuestRegister storage)) return null;
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
        if (TaskLocalSymbol(method) is null) return true;
        GuestRegister? token = LoadTaskLocalToken(context, method, block, instructions);
        return token is not null && Call(context, Release, token, null, block, instructions) is not null;
    }

    public static bool RetainTaskLocal(CSharpFunctionLoweringContext context,
        SemanticAsyncMethod method, int block, List<GuestInstruction> instructions)
    {
        if (TaskLocalSymbol(method) is null) return true;
        GuestRegister? token = LoadTaskLocalToken(context, method, block, instructions);
        return token is not null && Call(context, Retain, token, null, block, instructions) is not null;
    }

    public static bool TransferTaskLocalToContinuation(CSharpFunctionLoweringContext context,
        SemanticAsyncMethod method, GuestRegister continuationToken, int block,
        List<GuestInstruction> instructions)
    {
        if (TaskLocalSymbol(method) is null) return true;
        GuestRegister? token = LoadTaskLocalToken(context, method, block, instructions);
        GuestRegister? accepted = context.CreateTemporary(IntTypeId, block);
        if (token is null || accepted is null) return false;
        instructions.Add(new("call", accepted.Id, new[] { token.Id, continuationToken.Id },
            RetainForContinuationImportId, null, null));
        return ReleaseTaskLocal(context, method, block, instructions);
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
