using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json.Nodes;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class WasmFunctionReferenceTests
{
    private const string Int = "type:int32";
    private const string A = "type:callback_a";
    private const string B = "type:callback_b";
    private const string R = "type:recursive";
    private const string Pair = "type:pair";
    private const string P = "type:pair_factory";
    private const string V = "type:mutator";
    private const string D = "type:double_callback";
    private const string Empty = "type:empty";
    private const string Address = "type:address";
    private const string Double = "type:float64";

    public static int Run()
    {
        GuestModule module = Create();
        Require(GuestModuleValidator.Validate(module).Succeeded, "function reference fixture must validate");
        byte[] json = GuestIrSerializer.Serialize(module);
        Require(json.SequenceEqual(GuestIrSerializer.Serialize(GuestIrSerializer.Deserialize(json))), "function contracts must round trip");
        WasmCompilationResult compiled = WasmModuleCompiler.Compile(module);
        Require(compiled.Succeeded, string.Join(" | ", compiled.Diagnostics.Select(item => item.Message)));
        Require(compiled.Bytes.SequenceEqual(WasmModuleCompiler.Compile(module).Bytes), "table layout must be deterministic");
        WasmArtifactInfo info = WasmArtifactInspector.Inspect(compiled.Bytes);
        Require(info.SectionIds.Contains((byte)4) && info.SectionIds.Contains((byte)9)
            && info.Exports.All(item => item.Kind != 1), "table must exist and remain private");
        WasmCompilationResult cooperative = WasmModuleCompiler.Compile(module, new(true, 4));
        Require(cooperative.Succeeded && cooperative.CooperativeSafepointAttestation is { RecursiveFunctionCount: 1, Verified: true },
            "indirect recursion must participate in cancellation coverage");
        GuestFunction mixed = Function("function:plus", new[] { Reg("value", Int) }, new[] { Reg("ref", A), Reg("result", Int) }, Int,
            new[] { Op("function_ref", "ref", Array.Empty<string>(), "function:plus"),
                Op("call", "result", new[] { "ref", "value" }, "function:apply") }, "result");
        WasmCompilationResult mutual = WasmModuleCompiler.Compile(module with
            { Functions = module.Functions.Select(function => function.Id == mixed.Id ? mixed : function).ToArray() }, new(true, 4));
        Require(mutual.Succeeded && mutual.CooperativeSafepointAttestation is { RecursiveFunctionCount: 3, Verified: true },
            "mixed direct and indirect call cycles must cover every recursive entry");
        foreach ((int schema, string version) in new[] { (1, "1.0"), (2, "1.1") })
        {
            JsonObject legacy = JsonNode.Parse(GuestIrSerializer.Serialize(WasmModuleCompilerTests.CreateMinimalModule() with
                { SchemaVersion = schema, IrVersion = version }))!.AsObject();
            legacy.Remove("function_references");
            Require(WasmModuleCompiler.Compile(GuestIrSerializer.Deserialize(System.Text.Encoding.UTF8.GetBytes(legacy.ToJsonString()))).Succeeded,
                "old JSON without the function_references field must still compile");
        }

        GuestFunctionReference first = module.FunctionReferences[0];
        GuestModule[] invalid =
        {
            module with { FunctionReferences = null! },
            module with { FunctionReferences = new GuestFunctionReference[] { null! } },
            module with { FunctionReferences = Array.Empty<GuestFunctionReference>() },
            module with { FunctionReferences = module.FunctionReferences.Append(first).ToArray() },
            Change(first with { ParameterTypeIds = null! }),
            Change(first with { ParameterTypeIds = new[] { "type:void" } }),
            Change(first with { ReturnTypeId = "type:missing" }),
            Change(first with { TargetFunctionIds = new[] { "function:absent" } }),
            Change(first with { TargetFunctionIds = new string[] { null! } }),
            Change(first with { TargetFunctionIds = null! }),
            Change(first with { TargetFunctionIds = new[] { "function:plus", "function:plus" } }),
            Change(first with { ParameterTypeIds = Array.Empty<string>() }),
            module with { SchemaVersion = 2, IrVersion = "1.1" },
            module with { SchemaVersion = 99 },
            ReplaceInstruction(module, "function:get_a", 0, Op("function_ref", "ref", Array.Empty<string>(), "function:recurse")),
            ReplaceInstruction(module, "function:apply", 0, Op("call_indirect", "result", new[] { "callback", "value" }, B)),
            ReplaceInstruction(module, "function:apply", 0, Op("call_indirect", "result", new[] { "callback" }, A)),
            ReplaceInstruction(module, "function:get_a", 0, new("constant", "ref", Array.Empty<string>(), null, null, new("int32", "1"))),
            ReplaceInstruction(module, "function:get_a", 0, Op("function_ref", "ref", new[] { "ref" }, "function:plus")),
            ReplaceInstruction(module, "function:apply", 0, Op("call_indirect", null, new[] { "callback", "value" }, A)),
            ReplaceInstruction(module, "function:apply", 0, new("call_indirect", "result", new[] { "callback", "value" }, A, "unchecked", null)),
            ReplaceInstruction(module, "function:get_a", 0, new("convert", "ref", new[] { "ref" }, null, null, null)),
            module with { Types = module.Types.Select(type => type.Id == A ? type with { Size = 8 } : type).ToArray() },
        };
        foreach (GuestModule candidate in invalid)
            Require(!GuestModuleValidator.Validate(candidate).Succeeded && !WasmModuleCompiler.Compile(candidate).Succeeded,
                "malformed function references must fail before code generation");

        Require(module.FunctionReferences.Single(item => item.TypeId == Empty).TargetFunctionIds.Count == 0,
            "empty target set must remain uninhabited");
        string? root = Environment.GetEnvironmentVariable("AVIDSCRIPT_FUNCTION_REFERENCE_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(root))
        {
            Directory.CreateDirectory(root);
            File.WriteAllBytes(Path.Combine(root, "function-references.wasm"), compiled.Bytes);
            File.WriteAllBytes(Path.Combine(root, "function-references-cooperative.wasm"), cooperative.Bytes);
        }
        return 7 + 2 + invalid.Length + 1;

        GuestModule Change(GuestFunctionReference reference) => module with
            { FunctionReferences = new[] { reference }.Concat(module.FunctionReferences.Skip(1)).ToArray() };
    }

    private static GuestModule Create()
    {
        GuestModule baseline = WasmModuleCompilerTests.CreateMinimalModule();
        List<GuestFunction> functions = new();
        foreach ((string name, string kind) in new[] { ("plus", "add"), ("minus", "subtract") })
            functions.Add(Function("function:" + name, new[] { Reg("value", Int) },
                new[] { Reg("ten", Int), Reg("result", Int) }, Int,
                new[] { Constant("ten", 10), new GuestInstruction("binary", "result", new[] { "value", "ten" }, null, kind, null) }, "result"));
        foreach ((string name, string type, string target) in new[] { ("get_a", A, "plus"), ("get_minus", A, "minus"), ("get_b", B, "plus") })
            functions.Add(Function("function:" + name, Array.Empty<GuestRegister>(), new[] { Reg("ref", type) }, type,
                new[] { Op("function_ref", "ref", Array.Empty<string>(), "function:" + target) }, "ref"));
        functions.Add(Function("function:apply", new[] { Reg("callback", A), Reg("value", Int) }, new[] { Reg("result", Int) }, Int,
            new[] { Op("call_indirect", "result", new[] { "callback", "value" }, A) }, "result"));
        functions.Add(Function("function:identity", new[] { Reg("callback", A) }, Array.Empty<GuestRegister>(), A,
            Array.Empty<GuestInstruction>(), "callback"));
        functions.Add(new GuestFunction("function:recurse", new[] { Reg("n", Int) },
            new[] { Reg("zero", Int), Reg("condition", Int), Reg("one", Int), Reg("next", Int), Reg("ref", R), Reg("nested", Int), Reg("result", Int) },
            Int, "entry", new[]
            {
                new GuestBasicBlock("entry", new[] { Constant("zero", 0), new GuestInstruction("binary", "condition", new[] { "n", "zero" }, null, "greater_than", null) },
                    new("branch_if", "condition", "step", "done", null)),
                new GuestBasicBlock("done", Array.Empty<GuestInstruction>(), new("return", null, null, null, "zero")),
                new GuestBasicBlock("step", new[] { Constant("one", 1), new GuestInstruction("binary", "next", new[] { "n", "one" }, null, "subtract", null),
                    Op("function_ref", "ref", Array.Empty<string>(), "function:recurse"), Op("call_indirect", "nested", new[] { "ref", "next" }, R),
                    new GuestInstruction("binary", "result", new[] { "nested", "n" }, null, "add", null) }, new("return", null, null, null, "result")),
            }));
        functions.Add(Function("function:make_pair", new[] { Reg("value", Int) }, new[] { Reg("pair", Pair) }, Pair,
            new[] { Op("stack_alloc", "pair", Array.Empty<string>()), Op("field_store", null, new[] { "pair", "value" }, "field:x") }, "pair"));
        functions.Add(Function("function:pair", new[] { Reg("value", Int) }, new[] { Reg("ref", P), Reg("pair", Pair), Reg("result", Int) }, Int,
            new[] { Op("function_ref", "ref", Array.Empty<string>(), "function:make_pair"), Op("call_indirect", "pair", new[] { "ref", "value" }, P),
                Op("field_load", "result", new[] { "pair" }, "field:x") }, "result"));
        functions.Add(Function("function:mutate", new[] { Reg("address", Address) }, new[] { Reg("original", Int), Reg("ten", Int), Reg("sum", Int) }, "type:void",
            new[] { Op("indirect_load", "original", new[] { "address" }, Int), Constant("ten", 10),
                new GuestInstruction("binary", "sum", new[] { "original", "ten" }, null, "add", null),
                Op("indirect_store", null, new[] { "address", "sum" }, Int) }, null));
        functions.Add(Function("function:byref", new[] { Reg("value", Int) }, new[] { Reg("address", Address), Reg("ref", V), Reg("result", Int) }, Int,
            new[] { Op("address_of", "address", Array.Empty<string>(), "value"), Op("function_ref", "ref", Array.Empty<string>(), "function:mutate"),
                Op("call_indirect", null, new[] { "ref", "address" }, V), Op("local_load", "result", Array.Empty<string>(), "value") }, "result"));
        functions.Add(Function("function:double_target", new[] { Reg("value", Double) }, new[] { Reg("result", Double) }, Double,
            new[] { new GuestInstruction("binary", "result", new[] { "value", "value" }, null, "add", null) }, "result"));
        functions.Add(Function("function:twice", new[] { Reg("value", Double) }, new[] { Reg("ref", D), Reg("result", Double) }, Double,
            new[] { Op("function_ref", "ref", Array.Empty<string>(), "function:double_target"), Op("call_indirect", "result", new[] { "ref", "value" }, D) }, "result"));
        functions.Add(Function("function:empty", new[] { Reg("ref", Empty), Reg("value", Int) }, new[] { Reg("result", Int) }, Int,
            new[] { Op("call_indirect", "result", new[] { "ref", "value" }, Empty) }, "result"));
        functions.Add(Function("function:null_call", new[] { Reg("value", Int) }, new[] { Reg("ref", A), Reg("result", Int) }, Int,
            new[] { new GuestInstruction("constant", "ref", Array.Empty<string>(), null, null, new("null", null)),
                Op("call_indirect", "result", new[] { "ref", "value" }, A) }, "result"));
        return baseline with
        {
            SchemaVersion = 3, IrVersion = "1.2",
            Types = baseline.Types.Concat(new[] { RefType(A), RefType(B), RefType(R), RefType(P), RefType(V), RefType(D), RefType(Empty),
                new GuestType(Address, "scalar", "i32", Array.Empty<GuestField>(), null, null, 4, 4),
                new GuestType(Double, "scalar", "f64", Array.Empty<GuestField>(), null, null, 8, 8),
                new GuestType(Pair, "struct", "memory", new[] { new GuestField("field:x", "X", Int, 0) }, null, null, 4, 4) }).ToArray(),
            Functions = functions.ToArray(),
            Exports = new[] { "get_a", "get_minus", "get_b", "apply", "identity", "recurse", "pair", "byref", "twice", "empty", "null_call" }.Select(name => new GuestExport(name, "function:" + name)).ToArray(),
            FunctionReferences = new[]
            {
                new GuestFunctionReference(A, new[] { Int }, Int, new[] { "function:plus", "function:minus" }),
                new GuestFunctionReference(B, new[] { Int }, Int, new[] { "function:plus" }),
                new GuestFunctionReference(R, new[] { Int }, Int, new[] { "function:recurse" }),
                new GuestFunctionReference(P, new[] { Int }, Pair, new[] { "function:make_pair" }),
                new GuestFunctionReference(V, new[] { Address }, "type:void", new[] { "function:mutate" }),
                new GuestFunctionReference(D, new[] { Double }, Double, new[] { "function:double_target" }),
                new GuestFunctionReference(Empty, new[] { Int }, Int, Array.Empty<string>()),
            },
        };
    }

    private static GuestType RefType(string id) => new(id, "function_ref", "i32", Array.Empty<GuestField>(), null, null, 4, 4);
    private static GuestRegister Reg(string id, string type) => new(id, type);
    private static GuestInstruction Op(string op, string? result, string[] operands, string? target = null) => new(op, result, operands, target, null, null);
    private static GuestInstruction Constant(string result, int value) => new("constant", result, Array.Empty<string>(), null, null,
        new("int32", value.ToString(System.Globalization.CultureInfo.InvariantCulture)));
    private static GuestFunction Function(string id, GuestRegister[] parameters, GuestRegister[] locals, string result,
        GuestInstruction[] instructions, string? returnValue) => new(id, parameters, locals, result, "entry",
            new[] { new GuestBasicBlock("entry", instructions, new("return", null, null, null, returnValue)) });
    private static GuestModule ReplaceInstruction(GuestModule module, string functionId, int index, GuestInstruction instruction) => module with
    {
        Functions = module.Functions.Select(function => function.Id != functionId ? function : function with
        {
            Blocks = function.Blocks.Select((block, ordinal) => ordinal != 0 ? block : block with
                { Instructions = block.Instructions.Select((item, i) => i == index ? instruction : item).ToArray() }).ToArray(),
        }).ToArray(),
    };
    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}
