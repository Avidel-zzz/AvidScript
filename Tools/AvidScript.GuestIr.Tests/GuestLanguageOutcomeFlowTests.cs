using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using AvidScript.GuestIr;

internal static class GuestLanguageOutcomeFlowTests
{
    private const string Int = "type:int32";
    private const string Outcome = "type:language_outcome";
    private const string Leaf = "function:outcome_leaf";
    private const string Caller = "function:outcome_caller";

    public static int Run()
    {
        DirectCallChecksStatusBeforeValue();
        IndirectCallChecksStatusBeforeValue();
        IR15CannotHideEffectfulFunctions();
        IR16RequiresOutcomeTypeList();
        MissingStatusBranchIsRejected();
        ValueBeforeStatusBranchIsRejected();
        ValueOnErrorPathIsRejected();
        MergedPathCannotReadValue();
        OutcomeCannotEscapeAsAnOperand();
        OutcomeCannotEscapeThroughAddressOf();
        OutcomeParameterIsRejected();
        HostOutcomeImportIsRejected();
        RawOutcomeExportIsRejected();
        return 13;
    }

    private static void DirectCallChecksStatusBeforeValue()
    {
        GuestModule module = CreateModule();
        AssertValid(module);
        byte[] first = GuestIrSerializer.Serialize(module);
        Check(first.SequenceEqual(GuestIrSerializer.Serialize(GuestIrSerializer.Deserialize(first))),
            "IR 16 outcome call flow must round-trip canonically");
        Check(GuestIrSerializer.Deserialize(first).SchemaVersion == 16,
            "outcome call flow must have a distinct version identity");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_LANGUAGE_OUTCOME_FLOW_IR_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output, "language-outcome-flow.guest.json"), first);
        }
    }

    private static void IndirectCallChecksStatusBeforeValue()
    {
        GuestModule module = CreateModule();
        GuestType referenceType = new("type:outcome_fn", "function_ref", "i32",
            Array.Empty<GuestField>(), null, null, 4, 4);
        GuestFunction caller = module.Functions.Single(function => function.Id == Caller);
        GuestBasicBlock entry = caller.Blocks[0];
        caller = caller with
        {
            Locals = caller.Locals.Append(new GuestRegister("target", referenceType.Id)).ToArray(),
            Blocks = ReplaceBlock(caller, entry with
            {
                Instructions = new[]
                {
                    Op("function_ref", "target", Array.Empty<string>(), Leaf),
                    Op("call_indirect", "result", new[] { "target", "input" }, referenceType.Id),
                    Load("status", "result", "status"),
                },
            }),
        };
        AssertValid(module with
        {
            Types = module.Types.Append(referenceType).ToArray(),
            Functions = ReplaceFunction(module, caller),
            FunctionReferences = new[]
            {
                new GuestFunctionReference(referenceType.Id, new[] { Int }, Outcome, new[] { Leaf }),
            },
        });
    }

    private static void IR15CannotHideEffectfulFunctions() =>
        AssertError(CreateModule() with { SchemaVersion = 15, IrVersion = "1.14" }, "ASIR1025");

    private static void IR16RequiresOutcomeTypeList() =>
        AssertError(CreateModule() with { LanguageOutcomeTypes = null }, "ASIR1024");

    private static void MissingStatusBranchIsRejected()
    {
        GuestModule module = CreateModule();
        GuestFunction caller = module.Functions.Single(function => function.Id == Caller);
        GuestBasicBlock entry = caller.Blocks[0] with
        {
            Terminator = new GuestTerminator("branch", null, "success", null, null),
        };
        AssertError(module with { Functions = ReplaceFunction(module, caller with
        {
            Blocks = ReplaceBlock(caller, entry),
        }) }, "ASIR1025");
    }

    private static void ValueBeforeStatusBranchIsRejected()
    {
        GuestModule module = CreateModule();
        GuestFunction caller = module.Functions.Single(function => function.Id == Caller);
        GuestBasicBlock entry = caller.Blocks[0] with
        {
            Instructions = caller.Blocks[0].Instructions.Append(Load("value", "result", "value")).ToArray(),
        };
        AssertError(module with { Functions = ReplaceFunction(module, caller with
        {
            Blocks = ReplaceBlock(caller, entry),
        }) }, "ASIR1025");
    }

    private static void ValueOnErrorPathIsRejected()
    {
        GuestModule module = CreateModule();
        GuestFunction caller = module.Functions.Single(function => function.Id == Caller);
        GuestBasicBlock error = caller.Blocks.Single(block => block.Id == "error") with
        {
            Instructions = new[] { Load("value", "result", "value") },
            Terminator = Return("value"),
        };
        AssertError(module with { Functions = ReplaceFunction(module, caller with
        {
            Blocks = ReplaceBlock(caller, error),
        }) }, "ASIR1025");
    }

    private static void MergedPathCannotReadValue()
    {
        GuestModule module = CreateModule();
        GuestFunction caller = module.Functions.Single(function => function.Id == Caller);
        GuestBasicBlock error = caller.Blocks.Single(block => block.Id == "error") with
        {
            Instructions = Array.Empty<GuestInstruction>(),
            Terminator = new GuestTerminator("branch", null, "success", null, null),
        };
        AssertError(module with { Functions = ReplaceFunction(module, caller with
        {
            Blocks = ReplaceBlock(caller, error),
        }) }, "ASIR1025");
    }

    private static void OutcomeCannotEscapeAsAnOperand()
    {
        GuestModule module = CreateModule();
        GuestFunction caller = module.Functions.Single(function => function.Id == Caller);
        GuestBasicBlock success = caller.Blocks.Single(block => block.Id == "success") with
        {
            Instructions = new[]
            {
                Op("copy", "duplicate", new[] { "result" }),
                Load("value", "result", "value"),
            },
        };
        caller = caller with
        {
            Locals = caller.Locals.Append(new GuestRegister("duplicate", Outcome)).ToArray(),
            Blocks = ReplaceBlock(caller, success),
        };
        AssertError(module with { Functions = ReplaceFunction(module, caller) }, "ASIR1025");
    }

    private static void HostOutcomeImportIsRejected()
    {
        GuestModule module = CreateModule();
        AssertError(module with
        {
            Imports = module.Imports.Append(new GuestImport("import:outcome", "env", "outcome",
                Array.Empty<string>(), Outcome)).ToArray(),
        }, "ASIR1025");
    }

    private static void OutcomeParameterIsRejected()
    {
        GuestModule module = CreateModule();
        GuestFunction caller = module.Functions.Single(function => function.Id == Caller);
        AssertError(module with { Functions = ReplaceFunction(module, caller with
        {
            Parameters = caller.Parameters.Append(new GuestRegister("unchecked", Outcome)).ToArray(),
        }) }, "ASIR1025");
    }

    private static void OutcomeCannotEscapeThroughAddressOf()
    {
        GuestModule module = CreateModule();
        GuestFunction caller = module.Functions.Single(function => function.Id == Caller);
        GuestBasicBlock success = caller.Blocks.Single(block => block.Id == "success") with
        {
            Instructions = new[]
            {
                Op("address_of", "alias", Array.Empty<string>(), "result"),
                Load("value", "result", "value"),
            },
        };
        caller = caller with
        {
            Locals = caller.Locals.Append(new GuestRegister("alias", "type:address")).ToArray(),
            Blocks = ReplaceBlock(caller, success),
        };
        AssertError(module with { Functions = ReplaceFunction(module, caller) }, "ASIR1025");
    }

    private static void RawOutcomeExportIsRejected()
    {
        GuestModule module = CreateModule();
        AssertError(module with
        {
            Exports = module.Exports.Append(new GuestExport("raw_outcome", Leaf)).ToArray(),
        }, "ASIR1025");
    }

    internal static GuestModule CreateModule()
    {
        GuestModule baseline = GuestLanguageOutcomeTypeTests.CreateModule();
        GuestFunction leaf = new(Leaf, new[] { Reg("input", Int) },
            new[] { Reg("out", Outcome), Reg("zero", Int), Reg("one", Int), Reg("answer", Int) },
            Outcome, "entry", new[]
            {
                Block("entry", new[]
                {
                    Op("stack_alloc", "out"), Constant("zero", 0), Constant("one", 1),
                    Constant("answer", 42),
                }, new GuestTerminator("branch_if", "input", "error", "success", null)),
                Block("success", new[]
                {
                    Store("out", "status", "zero"), Store("out", "value", "answer"),
                }, Return("out")),
                Block("error", new[]
                {
                    Store("out", "status", "one"), Store("out", "error_type", "one"),
                    Store("out", "source", "one"),
                }, Return("out")),
            });
        GuestFunction caller = new(Caller, new[] { Reg("input", Int) },
            new[] { Reg("result", Outcome), Reg("status", Int), Reg("value", Int),
                Reg("error_type", Int), Reg("source", Int), Reg("error_code", Int) },
            Int, "entry", new[]
            {
                Block("entry", new[]
                {
                    Op("call", "result", new[] { "input" }, Leaf),
                    Load("status", "result", "status"),
                }, new GuestTerminator("branch_if", "status", "error", "success", null)),
                Block("error", new[]
                {
                    Load("error_type", "result", "error_type"),
                    Load("source", "result", "source"),
                    new GuestInstruction("binary", "error_code",
                        new[] { "error_type", "source" }, null, "add", null),
                }, Return("error_code")),
                Block("success", new[] { Load("value", "result", "value") }, Return("value")),
            });
        return baseline with
        {
            SchemaVersion = 16,
            IrVersion = "1.15",
            Functions = new[] { leaf, caller },
            Exports = new[] { new GuestExport("checked_outcome", Caller) },
        };
    }

    private static GuestRegister Reg(string id, string typeId) => new(id, typeId);
    private static GuestInstruction Op(string op, string? result, string[]? operands = null,
        string? target = null) =>
        new(op, result, operands ?? Array.Empty<string>(), target, null, null);
    private static GuestInstruction Constant(string result, int value) =>
        new("constant", result, Array.Empty<string>(), null, null,
            new GuestConstant("int32", value.ToString(System.Globalization.CultureInfo.InvariantCulture)));
    private static GuestInstruction Store(string owner, string field, string value) =>
        Op("field_store", null, new[] { owner, value }, "field:" + field);
    private static GuestInstruction Load(string result, string owner, string field) =>
        Op("field_load", result, new[] { owner }, "field:" + field);
    private static GuestBasicBlock Block(string id, GuestInstruction[] instructions, GuestTerminator terminator) =>
        new(id, instructions, terminator);
    private static GuestTerminator Return(string value) => new("return", null, null, null, value);
    private static GuestBasicBlock[] ReplaceBlock(GuestFunction function, GuestBasicBlock replacement) =>
        function.Blocks.Select(block => block.Id == replacement.Id ? replacement : block).ToArray();
    private static GuestFunction[] ReplaceFunction(GuestModule module, GuestFunction replacement) =>
        module.Functions.Select(function => function.Id == replacement.Id ? replacement : function).ToArray();
    private static void AssertValid(GuestModule module)
    {
        GuestValidationResult result = GuestModuleValidator.Validate(module);
        Check(result.Succeeded, string.Join(" | ", result.Diagnostics.Select(item => item.Message)));
    }
    private static void AssertError(GuestModule module, string code)
    {
        GuestValidationResult result = GuestModuleValidator.Validate(module);
        Check(!result.Succeeded && result.Diagnostics.Any(item => item.Code == code),
            $"expected {code}: " + string.Join(" | ", result.Diagnostics.Select(item => item.Message)));
    }
    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}
