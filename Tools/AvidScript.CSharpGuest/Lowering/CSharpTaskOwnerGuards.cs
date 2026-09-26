using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

// Conditional ownership uses the existing numeric Task slots and Host ABI.
// Zero means this path never acquired the local; nonzero failures still trap.
internal static class CSharpTaskOwnerGuards
{
    public const string RetainFunctionId = "function:$async:task_owner:retain";
    public const string ReleaseFunctionId = "function:$async:task_owner:release";
    public const string TransferFunctionId = "function:$async:task_owner:transfer";

    private static string Function(string operation) => operation switch
    {
        "retain" => RetainFunctionId,
        "release" => ReleaseFunctionId,
        "transfer" => TransferFunctionId,
        _ => throw new ArgumentOutOfRangeException(nameof(operation)),
    };

    public static bool Required(SemanticAsyncMethod method) =>
        SemanticAsyncTaskOwnership.TryAnalyze(method, CSharpTaskResultAbi.TaskLocalSymbols(method),
            method.TaskLocalSymbolIds is not null, out var flow) && flow!.RequiresGuards;

    public static bool Initialize(CSharpFunctionLoweringContext context, SemanticAsyncMethod method,
        int block, List<GuestInstruction> instructions)
    {
        if (!Required(method)) return true;
        GuestRegister? zero = CSharpTaskResultAbi.Constant(context, CSharpTaskResultAbi.TokenTypeId, 0, block, instructions);
        if (zero is null) return false;
        foreach (string symbol in CSharpTaskResultAbi.TaskLocalSymbols(method))
        {
            if (!context.TryGetStorage(symbol, out GuestRegister storage)) return false;
            GuestRegister? value = context.CreateTemporary(storage.TypeId, block);
            if (value is null) return false;
            instructions.Add(new("convert", value.Id, new[] { zero.Id }, null, null, null));
            instructions.Add(new("local_store", null, new[] { value.Id }, storage.Id, null, null));
        }
        return true;
    }

    public static bool Call(CSharpFunctionLoweringContext context, SemanticAsyncMethod method,
        int command, GuestRegister token, int block, List<GuestInstruction> instructions)
    {
        if (!Required(method)) return CSharpTaskResultAbi.Call(context, command, token, null, block, instructions) is not null;
        instructions.Add(new("call", null, new[] { token.Id },
            Function(command == CSharpTaskResultAbi.Retain ? "retain" : "release"), null, null));
        return true;
    }

    public static void Transfer(GuestRegister token, GuestRegister continuation, List<GuestInstruction> instructions) =>
        instructions.Add(new("call", null, new[] { token.Id, continuation.Id }, Function("transfer"), null, null));

    public static IReadOnlyList<GuestFunction> Build(SemanticDocument document) =>
        document.AsyncMethods.Any(Required)
            ? new[] { Build("retain", CSharpTaskResultAbi.Retain), Build("release", CSharpTaskResultAbi.Release),
                Build("transfer", CSharpTaskResultAbi.Release) } : Array.Empty<GuestFunction>();

    private static GuestFunction Build(string operation, int command)
    {
        bool transfer = operation == "transfer";
        List<GuestRegister> parameters = new() { new("token", "type:int64") };
        if (transfer) parameters.Add(new("continuation", "type:int64"));
        List<GuestRegister> locals = new()
        {
            new("zero64", "type:int64"), new("present", "type:int32"),
            new("command", "type:int32"), new("zero32", "type:int32"),
            new("result", "type:int64"), new("accepted", "type:int32"),
        };
        List<GuestBasicBlock> blocks = new()
        {
            new("entry", new GuestInstruction[] {
                new("constant", "zero64", Array.Empty<string>(), null, null, new("int64", "0")),
                new("binary", "present", new[] { "token", "zero64" }, null, "not_equals", null),
            }, new("branch_if", "present", transfer ? "transfer" : "operate", "done", null)),
        };
        if (transfer)
        {
            locals.Add(new("transferred", "type:int32"));
            blocks.Add(new("transfer", new GuestInstruction[] {
                new("call", "transferred", new[] { "token", "continuation" },
                    CSharpTaskResultAbi.RetainForContinuationImportId, null, null),
            }, new("branch_if", "transferred", "operate", "failed", null)));
        }
        blocks.Add(new("operate", new GuestInstruction[] {
            new("constant", "command", Array.Empty<string>(), null, null,
                new("int32", command.ToString(System.Globalization.CultureInfo.InvariantCulture))),
            new("constant", "zero32", Array.Empty<string>(), null, null, new("int32", "0")),
            new("call", "result", new[] { "command", "token", "zero32", "zero32" }, CSharpTaskResultAbi.ImportId, null, null),
            new("binary", "accepted", new[] { "result", "zero64" }, null, "not_equals", null),
        }, new("branch_if", "accepted", "done", "failed", null)));
        blocks.Add(new("done", Array.Empty<GuestInstruction>(), new("return", null, null, null, null)));
        blocks.Add(new("failed", Array.Empty<GuestInstruction>(), new("trap", null, null, null, null)));
        return new(Function(operation), parameters, locals, "type:void", "entry", blocks);
    }
}
