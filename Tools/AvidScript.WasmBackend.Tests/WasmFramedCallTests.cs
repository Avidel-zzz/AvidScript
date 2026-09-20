using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json.Nodes;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class WasmFramedCallTests
{
    public static int Run()
    {
        GuestModule managed = FrameCalls(WithWideSignature(WasmManagedHeapTests.Create()));
        GuestModule borrowed = FrameCalls(WasmBorrowedReferenceTests.Create());
        foreach (var (module, name) in new[] { (managed, "framed-managed"), (borrowed, "framed-borrowed") })
        {
            Valid(module);
            WasmCompilationResult compiled = WasmModuleCompiler.Compile(module);
            Require(compiled.Succeeded, string.Join(" | ", compiled.Diagnostics.Select(item => item.Message)));
            Require(compiled.Bytes.SequenceEqual(WasmModuleCompiler.Compile(module).Bytes), "framed codegen determinism");
            byte[] json = GuestIrSerializer.Serialize(module);
            Require(json.SequenceEqual(GuestIrSerializer.Serialize(GuestIrSerializer.Deserialize(json))), "framed canonical roundtrip");
            WasmArtifactInfo artifact = WasmArtifactInspector.Inspect(compiled.Bytes);
            Require(artifact.Exports.Count == module.Exports.Count + module.FramedExports.Count + 1, "all framed exports emitted");
            Require(artifact.CustomSections.Single(section => section.Name == GuestCallFrameLayout.SectionName)
                .PayloadText.Contains(Layout(module, "frame:begin").SignatureSha256, StringComparison.Ordinal), "artifact binds layout signature");
            WasmCompilationResult cooperative = WasmModuleCompiler.Compile(module, new(true, 4));
            Require(cooperative.Succeeded && cooperative.CooperativeSafepointAttestation is { Verified: true }, "framed cooperative proof");
            if (name == "framed-managed") Require(cooperative.CooperativeSafepointAttestation!.RecursiveFunctionCount > 0,
                "recursion through frame adapters is included in the cancellation proof");
            string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_MANAGED_HEAP_WASM_DIR");
            if (!string.IsNullOrWhiteSpace(output))
            {
                Directory.CreateDirectory(output);
                File.WriteAllBytes(Path.Combine(output, name + ".wasm"), compiled.Bytes);
                File.WriteAllBytes(Path.Combine(output, name + ".guest-ir.json"), json);
            }
        }
        GuestCallFrameLayout wide = Layout(managed, "frame:wide");
        Require(wide.Parameters.Count == 16 && wide.Roots.Count == 1 && wide.ByteSize % 16 == 0,
            "wide mixed signature includes managed aggregate return roots and alignment");
        Require(Layout(borrowed, "frame:write").Roots.Count == 1, "ref object reserves caller pointee root");
        Require(Layout(managed, "frame:make").Roots.Count == 1, "managed scalar return reserves caller root");
        Require(Layout(managed with { Types = managed.Types.Reverse().ToArray() }, "frame:wide").SignatureSha256 == wide.SignatureSha256,
            "type discovery order cannot change the signature");
        GuestModule changed = managed with { Types = managed.Types.Select(type => type.Id == "type:node"
            ? type with { Fields = type.Fields.Select(field => field.Id == "number" ? field with { Name = "renamed" } : field).ToArray() } : type).ToArray() };
        Require(Layout(changed, "frame:wide").SignatureSha256 != wide.SignatureSha256, "reachable managed payload identity is hashed");
        GuestFramedExport first = borrowed.FramedExports.First(export => export.FunctionId == "write");
        Require(Layout(borrowed with { FramedExports = borrowed.FramedExports.Select(export => export == first
            ? export with { ParameterKinds = new[] { "out" } } : export).ToArray() }, first.Name).SignatureSha256
            != Layout(borrowed, first.Name).SignatureSha256, "ref and out have distinct signatures");

        GuestModule[] invalid =
        {
            managed with { SchemaVersion = 6, IrVersion = "1.5" },
            managed with { SchemaVersion = 9, IrVersion = "1.8" },
            managed with { FramedExports = null! },
            managed with { FramedExports = new GuestFramedExport[] { null! } },
            managed with { FramedExports = new[] { managed.FramedExports[0] with { ParameterKinds = null! } } },
            managed with { FramedExports = managed.FramedExports.Append(managed.FramedExports[0]).ToArray() },
            managed with { FramedExports = new[] { managed.FramedExports[0] with { Name = "memory" } } },
            managed with { FramedExports = new[] { managed.FramedExports[0] with { Name = "avid_on_begin_play" } } },
            managed with { FramedExports = new[] { managed.FramedExports[0] with { FunctionId = "absent" } } },
            borrowed with { FramedExports = borrowed.FramedExports.Select(export => export == first
                ? export with { ParameterKinds = new[] { "value" } } : export).ToArray() },
            borrowed with { FramedExports = borrowed.FramedExports.Select(export => export == first
                ? export with { ParameterKinds = new[] { "unknown" } } : export).ToArray() },
            managed with { Exports = managed.Exports.Append(new GuestExport("raw", "make")).ToArray() },
            borrowed with { Exports = borrowed.Exports.Append(new GuestExport("raw", "write")).ToArray() },
            managed with { Functions = managed.Functions.Select(function => function.Id == "noise" ? function with
            { Blocks = function.Blocks.Select(block => block with { Instructions = block.Instructions.Select(instruction => instruction.Op == "call_framed"
                ? instruction with { OperandIds = Array.Empty<string>() } : instruction).ToArray() }).ToArray() } : function).ToArray() },
        };
        foreach (GuestModule candidate in invalid)
            Require(!GuestModuleValidator.Validate(candidate).Succeeded && !WasmModuleCompiler.Compile(candidate).Succeeded,
                "malformed frame or raw-reference escape reached codegen");
        GuestModule legacy = WasmBorrowedReferenceTests.Create();
        JsonObject legacyJson = JsonNode.Parse(GuestIrSerializer.Serialize(legacy))!.AsObject();
        legacyJson.Remove("framed_exports");
        Valid(GuestIrSerializer.Deserialize(System.Text.Encoding.UTF8.GetBytes(legacyJson.ToJsonString())));
        return 22 + invalid.Length;
    }

    private static GuestModule FrameCalls(GuestModule module)
    {
        Dictionary<string, GuestType> types = module.Types.ToDictionary(type => type.Id);
        HashSet<string> functions = module.Functions.Select(function => function.Id).ToHashSet(StringComparer.Ordinal);
        GuestInstruction Call(GuestInstruction instruction)
        {
            if (instruction.Op == "call" && functions.Contains(instruction.TargetId!))
                return instruction with { Op = "call_framed", TargetId = "frame:" + instruction.TargetId };
            // The borrowed fixture's only delegate target receives two aliases. Route
            // that invocation through the new adapter too, so the frame itself (not
            // merely a nested scalar read) must preserve their shared location.
            if (instruction.Op == "call_indirect")
            {
                GuestFunctionReference reference = module.FunctionReferences.Single(reference => reference.TypeId == instruction.TargetId);
                if (reference.TargetFunctionIds.Count == 1
                    && reference.ParameterTypeIds.Any(id => types[id].Kind == GuestBorrowedReference.Kind))
                    return instruction with { Op = "call_framed", TargetId = "frame:" + reference.TargetFunctionIds[0],
                        OperandIds = instruction.OperandIds.Skip(1).ToArray() };
            }
            return instruction;
        }
        return module with
        {
            SchemaVersion = 7, IrVersion = "1.6",
            FramedExports = module.Functions.Select(function => new GuestFramedExport("frame:" + function.Id, function.Id,
                function.Parameters.Select(parameter => types[parameter.TypeId].Kind == GuestBorrowedReference.Kind ? "ref" : "value").ToArray())).ToArray(),
            Functions = module.Functions.Select(function => function with { Blocks = function.Blocks.Select(block => block with
            { Instructions = block.Instructions.Select(Call).ToArray() }).ToArray() }).ToArray(),
        };
    }

    private static GuestModule WithWideSignature(GuestModule module)
    {
        const string i32 = "type:int32", i64 = "wide_i64", f64 = "wide_f64", reference = "type:node_ref", box = "type:box";
        GuestRegister[] parameters = Enumerable.Range(0, 12).Select(index => new GuestRegister("n" + index, i32))
            .Concat(new[] { new GuestRegister("obj", reference), new GuestRegister("box", box), new GuestRegister("long", i64), new GuestRegister("double", f64) }).ToArray();
        List<GuestRegister> locals = new() { new("objValue", i32), new("tag", i32), new("longValue", i32), new("doubleValue", i32) };
        List<GuestInstruction> ops = new()
        {
            new("managed_collect", null, Array.Empty<string>(), null, null, null),
            new("managed_get", "objValue", new[] { "obj" }, "number", null, null),
            new("field_load", "tag", new[] { "box" }, "tag", null, null),
            new("convert", "longValue", new[] { "long" }, null, null, null),
            new("convert", "doubleValue", new[] { "double" }, null, null, null),
        };
        string sum = "n0";
        foreach (string operand in parameters.Skip(1).Take(11).Select(parameter => parameter.Id).Concat(new[] { "objValue", "tag", "longValue", "doubleValue" }))
        {
            string next = "sum" + locals.Count; locals.Add(new(next, i32));
            ops.Add(new("binary", next, new[] { sum, operand }, null, "add", null)); sum = next;
        }
        ops.Add(new("field_store", null, new[] { "box", sum }, "tag", null, null));
        GuestFunction wide = new("wide", parameters, locals, box, "entry",
            new[] { new GuestBasicBlock("entry", ops, new("return", null, null, null, "box")) });
        GuestFunction begin = module.Functions.Single(function => function.Id == "begin");
        List<GuestInstruction> entry = new();
        foreach (GuestInstruction instruction in begin.Blocks[0].Instructions)
        {
            if (instruction.Op == "global_store")
            {
                entry.Add(new("binary", "wideSum", new[] { "sum", "wideTag" }, null, "add", null));
                entry.Add(instruction with { OperandIds = new[] { "wideSum" } }); continue;
            }
            entry.Add(instruction);
            if (instruction.TargetId != "boxer") continue;
            entry.Add(new("constant", "longArg", Array.Empty<string>(), null, null, new("int64", "19")));
            entry.Add(new("constant", "doubleArg", Array.Empty<string>(), null, null, new("float64", "23.5")));
            entry.Add(new("call", "wideBox", Enumerable.Repeat("n", 12).Concat(new[] { "deep", "box", "longArg", "doubleArg" }).ToArray(), "wide", null, null));
            entry.Add(new("field_load", "wideTag", new[] { "wideBox" }, "tag", null, null));
        }
        begin = begin with { Locals = begin.Locals.Concat(new[] { new GuestRegister("longArg", i64), new("doubleArg", f64),
            new("wideBox", box), new("wideTag", i32), new("wideSum", i32) }).ToArray(),
            Blocks = new[] { begin.Blocks[0] with { Instructions = entry } } };
        return module with { Types = module.Types.Concat(new[] { new GuestType(i64, "scalar", "i64", Array.Empty<GuestField>(), null, null, 8, 8),
            new(f64, "scalar", "f64", Array.Empty<GuestField>(), null, null, 8, 8) }).ToArray(),
            Functions = module.Functions.Select(function => function.Id == "begin" ? begin : function).Append(wide).ToArray() };
    }

    private static GuestCallFrameLayout Layout(GuestModule module, string name)
    {
        GuestFramedExport export = module.FramedExports.Single(export => export.Name == name);
        return GuestCallFrameLayout.Create(export, module.Functions.Single(function => function.Id == export.FunctionId),
            module.Types.ToDictionary(type => type.Id), module.FunctionReferences);
    }
    private static void Valid(GuestModule module) => Require(GuestModuleValidator.Validate(module).Succeeded,
        string.Join(" | ", GuestModuleValidator.Validate(module).Diagnostics.Select(item => item.Message)));
    private static void Require(bool value, string message) { if (!value) throw new InvalidOperationException(message); }
}
