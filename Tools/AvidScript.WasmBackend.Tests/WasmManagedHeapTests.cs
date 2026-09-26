using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class WasmManagedHeapTests
{
    private const string I = "type:int32", V = "type:void", R = "type:node_ref", Node = "type:node", Box = "type:box", F = "type:identity", B = "type:bool";
    public static int Run()
    {
        CheckNativeWireContract();
        GuestModule module = Create();
        Require(GuestModuleValidator.Validate(module).Succeeded, string.Join(" | ", GuestModuleValidator.Validate(module).Diagnostics.Select(item => item.Message)));
        WasmCompilationResult compiled = WasmModuleCompiler.Compile(module);
        Require(compiled.Succeeded, string.Join(" | ", compiled.Diagnostics.Select(item => item.Message)));
        Require(compiled.Bytes.SequenceEqual(WasmModuleCompiler.Compile(module).Bytes), "managed layout and root insertion must be deterministic");
        byte[] json = GuestIrSerializer.Serialize(module);
        Require(json.SequenceEqual(GuestIrSerializer.Serialize(GuestIrSerializer.Deserialize(json))), "managed IR must round trip");
        WasmCompilationResult cooperative = WasmModuleCompiler.Compile(module, new(true, 4));
        Require(cooperative.Succeeded && cooperative.CooperativeSafepointAttestation is { Verified: true, RecursiveFunctionCount: > 0 }, "managed recursion must retain cooperative proof");
        Require(WasmArtifactInspector.Inspect(compiled.Bytes).Imports.Count == 1, "only the declared heap import is needed");
        GuestModule[] invalid =
        {
            module with { SchemaVersion = 3, IrVersion = "1.2" },
            module with { SchemaVersion = GuestModuleValidator.CurrentSchemaVersion + 1, IrVersion = GuestModuleValidator.CurrentIrVersion },
            module with { Imports = Array.Empty<GuestImport>() },
            module with { Imports = new[] { module.Imports[0] with { ParameterTypeIds = new[] { I } } } },
            module with { Imports = new[] { module.Imports[0] with { Module = "env" } } },
            module with { Exports = module.Exports.Append(new GuestExport("unsafe_return", "make")).ToArray() },
            module with { Exports = module.Exports.Append(new GuestExport("unsafe_argument", "identity")).ToArray() },
            module with { Globals = module.Globals.Append(new GuestGlobal("unsafe_root", R, true, new("null", null))).ToArray() },
            module with { Globals = module.Globals.Append(new GuestGlobal("unsafe_aggregate_root", Box, true, new("zero", null))).ToArray() },
            module with { Types = module.Types.Select(type => type.Id == R ? type with { ElementTypeId = I } : type).ToArray() },
            module with { Types = module.Types.Select(type => type.Id == R ? type with { Size = 4 } : type).ToArray() },
            module with { Types = module.Types.Append(new GuestType("unsafe_array", "array", "i32", Array.Empty<GuestField>(), R, null, 4, 4)).ToArray() },
            ReplaceMake(module, new("constant", "obj", Array.Empty<string>(), null, null, new("uint64", "1"))),
            ReplaceMake(module, Op("managed_new", "obj", new[] { "n" })),
            ReplaceMake(module, Op("managed_new", null, Array.Empty<string>())),
            ReplaceMake(module, new("managed_new", "obj", Array.Empty<string>(), null, "unsafe", null)),
            ReplaceMake(module, Op("managed_get", "obj", new[] { "n" }, "next")),
            ReplaceMake(module, Op("managed_get", "obj", new[] { "obj" }, "number")),
            ReplaceMake(module, Op("managed_set", null, new[] { "obj", "n" }, "next")),
            ReplaceMake(module, Op("managed_set", null, new[] { "obj", "n" }, "missing")),
            ReplaceMake(module, Op("managed_collect", "obj", Array.Empty<string>())),
            ReplaceMake(module, Op("convert", "obj", new[] { "n" })),
            ReplaceMake(WithAddress(module), Op("address_of", "address", Array.Empty<string>(), "obj")),
            ReplaceMake(WithAddress(module), Op("indirect_store", null, new[] { "address", "obj" }, R)),
            ReplaceMake(module, Op("managed_unknown", "obj", Array.Empty<string>())),
        };
        foreach (GuestModule candidate in invalid)
            Require(!GuestModuleValidator.Validate(candidate).Succeeded && !WasmModuleCompiler.Compile(candidate).Succeeded, "invalid managed contract reached codegen");
        foreach (GuestModule alias in invalid.Skip(invalid.Length - 3).Take(2))
            Require(GuestModuleValidator.Validate(alias).Diagnostics.Any(item => item.Code == "ASIR1013"), "untraced alias must fail the managed lifetime contract");
        GuestModule trap = module with { Functions = module.Functions.Select(function => function.Id == "begin"
            ? function with { Blocks = new[] { function.Blocks[0] with { Terminator = new("trap", null, null, null, null) } } } : function).ToArray() };
        WasmCompilationResult trapCompiled = WasmModuleCompiler.Compile(trap);
        Require(trapCompiled.Succeeded, "trap fixture must compile");
        GuestModule erased = WithErasedCasts(module, false);
        GuestModule wrongCast = WithErasedCasts(module, true);
        Require(GuestModuleValidator.Validate(erased).Succeeded, string.Join(" | ", GuestModuleValidator.Validate(erased).Diagnostics.Select(item => item.Message)));
        WasmCompilationResult erasedCompiled = WasmModuleCompiler.Compile(erased);
        WasmCompilationResult wrongCompiled = WasmModuleCompiler.Compile(wrongCast);
        Require(erasedCompiled.Succeeded && wrongCompiled.Succeeded, "checked casts compile: "
            + string.Join(" | ", erasedCompiled.Diagnostics.Concat(wrongCompiled.Diagnostics).Select(item => item.Message)));
        Require(GuestIrSerializer.Serialize(erased).SequenceEqual(GuestIrSerializer.Serialize(GuestIrSerializer.Deserialize(GuestIrSerializer.Serialize(erased)))),
            "erased shape and cast op round trip");
        GuestModule[] invalidCasts =
        {
            erased with { SchemaVersion = 4, IrVersion = "1.3" },
            ReplaceMake(erased, Op("managed_new", "erased")),
            ReplaceMake(erased, Op("managed_cast", "erased", new[] { "n" })),
            ReplaceMake(erased, Op("managed_cast", "n", new[] { "obj" })),
            ReplaceMake(erased, Op("managed_cast", "obj")),
            ReplaceMake(erased, Op("managed_cast", "obj", new[] { "erased" }, "forged")),
            ReplaceMake(erased, Op("managed_get", "n", new[] { "erased" }, "number")),
        };
        foreach (GuestModule invalidCast in invalidCasts)
            Require(!GuestModuleValidator.Validate(invalidCast).Succeeded && !WasmModuleCompiler.Compile(invalidCast).Succeeded,
                "erased allocation, scalar conversion, untyped access and downgraded casts must fail closed");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_MANAGED_HEAP_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output); File.WriteAllBytes(Path.Combine(output, "managed.wasm"), compiled.Bytes);
            File.WriteAllBytes(Path.Combine(output, "managed-trap.wasm"), trapCompiled.Bytes);
            File.WriteAllBytes(Path.Combine(output, "managed-cooperative.wasm"), cooperative.Bytes);
            File.WriteAllBytes(Path.Combine(output, "managed-erased.wasm"), erasedCompiled.Bytes);
            File.WriteAllBytes(Path.Combine(output, "managed-erased-wrong.wasm"), wrongCompiled.Bytes);
            File.WriteAllBytes(Path.Combine(output, "managed.guest-ir.json"), json);
        }
        return invalid.Length + 4 + invalidCasts.Length + 2;
    }

    private static GuestModule WithErasedCasts(GuestModule module, bool wrong)
    {
        const string erased = "type:erased", other = "type:other_ref";
        return module with
        {
            SchemaVersion = 5, IrVersion = "1.4",
            Types = module.Types.Concat(new[]
            {
                new GuestType(erased, "managed_ref", "i64", Array.Empty<GuestField>(), null, null, 8, 8),
                new GuestType(other, "managed_ref", "i64", Array.Empty<GuestField>(), Node, null, 8, 8),
            }).ToArray(),
            Functions = module.Functions.Select(function => function.Id != "make" ? function : function with
            {
                Locals = function.Locals.Concat(new[] { Reg("erased", erased), Reg("checked", R), Reg("wrong", other),
                    Reg("emptyErased", erased), Reg("emptyTyped", R) }).ToArray(),
                Blocks = new[] { function.Blocks[0] with { Instructions = new[]
                {
                    new GuestInstruction("constant", "emptyErased", Array.Empty<string>(), null, null, new("null", null)),
                    Op("managed_cast", "emptyTyped", new[] { "emptyErased" }), // null must not dereference
                    Op("managed_new", "obj"), Op("managed_set", null, new[] { "obj", "n" }, "number"),
                    Op("managed_cast", "erased", new[] { "obj" }),
                    Op("local_store", null, new[] { "emptyTyped" }, "obj"),
                    Op("managed_collect"), // only the erased reference keeps the object alive
                    Op("managed_cast", wrong ? "wrong" : "checked", new[] { "erased" }),
                    Op("local_store", null, new[] { wrong ? "emptyTyped" : "checked" }, "obj"),
                } } },
            }).ToArray(),
        };
    }

    private static void CheckNativeWireContract()
    {
        const string relative = "Source/AvidScriptCore/Public/AvidScriptManagedHeapAbi.h";
        DirectoryInfo? directory = new(AppContext.BaseDirectory);
        while (directory is not null && !File.Exists(Path.Combine(directory.FullName, relative))) directory = directory.Parent;
        Require(directory is not null, "native managed heap ABI header must be available to contract tests");
        string header = File.ReadAllText(Path.Combine(directory!.FullName, relative));
        Require(header.Contains($"ImportName[] = \"{GuestManagedHeap.ImportName}\"", StringComparison.Ordinal), "heap import identity drift");
        Require(Regex.IsMatch(header, $@"\bMagic\s*=\s*0x{GuestManagedHeap.Magic:x}\s*;", RegexOptions.IgnoreCase), "heap magic drift");
        foreach (var (name, value) in new[] { ("MaxLayouts", GuestManagedHeap.MaxLayouts),
                     ("MaxReferencesPerLayout", GuestManagedHeap.MaxReferencesPerLayout), ("MaxTotalReferences", GuestManagedHeap.MaxTotalReferences),
                     ("MaxStaticSlots", GuestManagedHeap.MaxStaticSlots) })
            Require(Regex.IsMatch(header, $@"\b{name}\s*=\s*{value}\s*;"), $"heap {name} drift");
        foreach (GuestManagedHeapCommand command in Enum.GetValues<GuestManagedHeapCommand>())
            Require(Regex.IsMatch(header, $@"\b{command}\s*=\s*{(int)command}\s*[,}}]"), $"heap command {command} drift");
    }

    internal static GuestModule Create()
    {
        GuestModule basis = WasmModuleCompilerTests.CreateMinimalModule();
        GuestType[] declarations = basis.Types.Concat(new[]
        {
            new GuestType(B, "scalar", "i32", Array.Empty<GuestField>(), null, null, 1, 1),
            new GuestType(R, "managed_ref", "i64", Array.Empty<GuestField>(), Node, null, 8, 8),
            new GuestType(F, "function_ref", "i32", Array.Empty<GuestField>(), null, null, 4, 4),
            new GuestType(Box, "struct", "memory", new[] { Field("link", R), Field("tag", I) }, null, null, 0, 1),
            new GuestType(Node, "struct", "memory", new[] { Field("number", I), Field("next", R), Field("box", Box) }, null, null, 0, 1),
        }).ToArray();
        GuestTypeLayoutResult types = GuestDataLayout.ComputeTypes(declarations); Require(types.Succeeded, "recursive managed payload layout");
        GuestGlobal[] globals = { new("result", I, true, new("int32", "0")) };
        GuestLayoutResult layout = GuestLayoutBuilder.Build(types.Types, globals, Array.Empty<GuestDataSegment>());
        Require(layout.Succeeded && layout.Layout!.StateSlots.Single().Offset == 16, "fixture result slot contract");
        GuestFunction make = Fn("make", new[] { Reg("n", I) }, new[] { Reg("obj", R) }, R,
            new[] { Op("managed_new", "obj"), Op("managed_set", null, new[] { "obj", "n" }, "number"), Op("managed_collect") }, "obj");
        GuestFunction noise = Fn("noise", Array.Empty<GuestRegister>(), new[] { Reg("junk", R), Reg("n", I) }, V,
            new[] { Num("n", 17), Op("call", "junk", new[] { "n" }, "make"), Op("managed_collect") });
        GuestFunction identity = Fn("identity", new[] { Reg("obj", R) }, Array.Empty<GuestRegister>(), R, new[] { Op("managed_collect") }, "obj");
        GuestFunction boxer = Fn("boxer", new[] { Reg("obj", R) }, new[] { Reg("box", Box), Reg("tag", I) }, Box,
            new[] { Num("tag", 7), Op("field_store", null, new[] { "box", "obj" }, "link"), Op("field_store", null, new[] { "box", "tag" }, "tag") }, "box");
        GuestFunction mutate = Fn("mutate", new[] { Reg("box", Box) }, new[] { Reg("nil", R), Reg("tag", I) }, I,
            new[] { new GuestInstruction("constant", "nil", Array.Empty<string>(), null, null, new("null", null)),
                Op("field_store", null, new[] { "box", "nil" }, "link"), Op("managed_collect"), Op("field_load", "tag", new[] { "box" }, "tag") }, "tag");
        GuestFunction bounce = new("bounce", new[] { Reg("obj", R), Reg("depth", I) },
            new[] { Reg("zero", I), Reg("one", I), Reg("next", I), Reg("condition", B), Reg("returned", R) }, R, "entry", new[]
            {
                new GuestBasicBlock("entry", new[] { Num("zero", 0), Num("one", 1), new GuestInstruction("binary", "condition", new[] { "depth", "zero" }, null, "greater_than", null) }, new("branch_if", "condition", "more", "done", null)),
                new GuestBasicBlock("done", new[] { Op("managed_collect") }, Ret("obj")),
                new GuestBasicBlock("more", new[] { new GuestInstruction("binary", "next", new[] { "depth", "one" }, null, "subtract", null),
                    Op("call", null, Array.Empty<string>(), "noise"), Op("call", "returned", new[] { "obj", "next" }, "bounce") }, Ret("returned")),
            });
        GuestFunction stress = new("stress", Array.Empty<GuestRegister>(), new[] { Reg("index", I), Reg("limit", I), Reg("one", I), Reg("next", I), Reg("condition", B), Reg("temporary", R) }, V, "entry", new[]
        {
            new GuestBasicBlock("entry", new[] { Num("index", 0), Num("limit", 128), Num("one", 1) }, new("branch", null, "check", null, null)),
            new GuestBasicBlock("check", new[] { new GuestInstruction("binary", "condition", new[] { "index", "limit" }, null, "less_than", null) }, new("branch_if", "condition", "body", "done", null)),
            new GuestBasicBlock("body", new[] { Op("call", "temporary", new[] { "index" }, "make"), new GuestInstruction("binary", "next", new[] { "index", "one" }, null, "add", null),
                Op("local_store", null, new[] { "next" }, "index") }, new("branch", null, "check", null, null)),
            new GuestBasicBlock("done", Array.Empty<GuestInstruction>(), Ret()),
        });
        GuestFunction begin = Fn("begin", Array.Empty<GuestRegister>(), new[]
        {
            Reg("n", I), Reg("depth", I), Reg("a", R), Reg("indirect", R), Reg("deep", R), Reg("callback", F),
            Reg("box", Box), Reg("loaded", Box), Reg("empty", Box), Reg("holder", R), Reg("nil", R), Reg("tag", I), Reg("reloaded", R), Reg("number", I), Reg("sum", I),
        }, V, new[]
        {
            Num("n", 42), Num("depth", 8), Op("call", "a", new[] { "n" }, "make"), Op("call", null, Array.Empty<string>(), "noise"),
            Op("function_ref", "callback", Array.Empty<string>(), "identity"), Op("call_indirect", "indirect", new[] { "callback", "a" }, F),
            Op("call", "deep", new[] { "indirect", "depth" }, "bounce"), Op("call", "box", new[] { "deep" }, "boxer"),
            Op("managed_new", "holder"), Op("managed_set", null, new[] { "holder", "box" }, "box"),
            Op("managed_set", null, new[] { "holder", "holder" }, "next"), // Self cycle: tracing must not leak it.
            new GuestInstruction("constant", "nil", Array.Empty<string>(), null, null, new("null", null)),
            new GuestInstruction("constant", "empty", Array.Empty<string>(), null, null, new("zero", null)),
            Op("local_store", null, new[] { "nil" }, "a"), Op("local_store", null, new[] { "nil" }, "indirect"), Op("local_store", null, new[] { "nil" }, "deep"),
            Op("local_store", null, new[] { "empty" }, "box"), Op("managed_collect"),
            Op("managed_get", "loaded", new[] { "holder" }, "box"), Op("call", "tag", new[] { "loaded" }, "mutate"),
            Op("call", null, Array.Empty<string>(), "stress"), Op("field_load", "reloaded", new[] { "loaded" }, "link"),
            Op("managed_get", "number", new[] { "reloaded" }, "number"), new GuestInstruction("binary", "sum", new[] { "number", "tag" }, null, "add", null),
            Op("global_store", null, new[] { "sum" }, "result"),
        });
        return basis with
        {
            SchemaVersion = 4, IrVersion = "1.3", Types = types.Types, Globals = globals, MemoryLayout = layout.Layout!,
            Imports = new[] { new GuestImport("heap", GuestManagedHeap.ImportModule, GuestManagedHeap.ImportName, new[] { I, I, I, I }, I) },
            Functions = new[] { begin, make, noise, identity, boxer, mutate, bounce, stress },
            Exports = new[] { new GuestExport("avid_on_begin_play", "begin") },
            FunctionReferences = new[] { new GuestFunctionReference(F, new[] { R }, R, new[] { "identity" }) },
        };
    }
    private static GuestModule ReplaceMake(GuestModule module, GuestInstruction replacement) => module with
    {
        Functions = module.Functions.Select(function => function.Id != "make" ? function : function with
        { Blocks = new[] { function.Blocks[0] with { Instructions = new[] { replacement }.Concat(function.Blocks[0].Instructions.Skip(1)).ToArray() } } }).ToArray()
    };
    private static GuestModule WithAddress(GuestModule module) => module with
    {
        Types = module.Types.Append(new GuestType("type:address", "scalar", "i32", Array.Empty<GuestField>(), null, null, 4, 4)).ToArray(),
        Functions = module.Functions.Select(function => function.Id != "make" ? function : function with
        { Locals = function.Locals.Append(Reg("address", "type:address")).ToArray() }).ToArray(),
    };
    private static GuestField Field(string id, string type) => new(id, id, type, 0);
    private static GuestRegister Reg(string id, string type) => new(id, type);
    private static GuestInstruction Op(string op, string? result = null, string[]? args = null, string? target = null) => new(op, result, args ?? Array.Empty<string>(), target, null, null);
    private static GuestInstruction Num(string result, int value) => new("constant", result, Array.Empty<string>(), null, null, new("int32", value.ToString(System.Globalization.CultureInfo.InvariantCulture)));
    private static GuestTerminator Ret(string? value = null) => new("return", null, null, null, value);
    private static GuestFunction Fn(string id, GuestRegister[] parameters, GuestRegister[] locals, string result, GuestInstruction[] ops, string? value = null) => new(id, parameters, locals, result, "entry", new[] { new GuestBasicBlock("entry", ops, Ret(value)) });
    private static void Require(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }
}
