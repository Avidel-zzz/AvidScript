using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json.Nodes;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class WasmHostDispatchTests
{
    public static int Run(GuestModule source)
    {
        int passed = 0;
        void Check(bool condition, string message)
        {
            if (!condition) throw new InvalidOperationException(message);
            ++passed;
        }
        GuestFunction wide = source.Functions.Single(function => function.Id == "wide");
        GuestFunction stub = wide with { Id = "dispatch:body", Locals = Array.Empty<GuestRegister>(),
            Blocks = new[] { new GuestBasicBlock(wide.EntryBlockId, Array.Empty<GuestInstruction>(), new("trap", null, null, null, null)) } };
        GuestFunction implementation = wide with { Id = "dispatch:override",
            Locals = wide.Locals.Append(new GuestRegister("recursiveResult", wide.ReturnTypeId)).ToArray(),
            Blocks = wide.Blocks.Select(block => block with { Instructions = block.Instructions.Append(
                new GuestInstruction("call_framed", "recursiveResult", wide.Parameters.Select(parameter => parameter.Id).ToArray(),
                    "frame:dispatch", null, null)).ToArray() }).ToArray() };
        GuestImport Import(string name) => new(name, "frame_test", name, new[] { "type:int32", "type:int32" }, "type:int32");
        var kinds = source.FramedExports.Single(export => export.Name == "frame:wide").ParameterKinds;
        GuestFramedExport route = new("frame:dispatch", stub.Id, kinds) { HostImportId = "dispatch:import",
            HostDispatchTargets = new GuestHostDispatchTarget[] { new(0, "frame:wide"), new(uint.MaxValue, "frame:override") } };
        GuestModule module = source with { SchemaVersion = 10, IrVersion = "1.9",
            Imports = source.Imports.Concat(new[] { Import("dispatch:import"), Import("base:import"), Import("override:import") }).ToArray(),
            Functions = source.Functions.Concat(new[] { stub, implementation }).ToArray(),
            FramedExports = source.FramedExports.Select(export => export.Name == "frame:wide"
                ? export with { HostImportId = "base:import" } : export).Concat(new[] {
                    new GuestFramedExport("frame:override", implementation.Id, kinds) { HostImportId = "override:import" }, route }).ToArray() };
        Check(GuestModuleValidator.Validate(module).Succeeded, "host dispatch contract must validate");
        byte[] json = GuestIrSerializer.Serialize(module);
        Check(json.SequenceEqual(GuestIrSerializer.Serialize(GuestIrSerializer.Deserialize(json))), "host dispatch canonical roundtrip");
        foreach (string field in new[] { "selector", "export_name" })
        {
            var missing = JsonNode.Parse(json)!.AsObject();
            var target = missing["framed_exports"]!.AsArray().Last()!["host_dispatch_targets"]![0]!.AsObject();
            target.Remove(field);
            bool rejected = false;
            try { GuestIrSerializer.Deserialize(System.Text.Encoding.UTF8.GetBytes(missing.ToJsonString())); }
            catch (System.IO.InvalidDataException exception) when (exception.InnerException is System.Text.Json.JsonException) { rejected = true; }
            Check(rejected, "missing dispatch identity must not deserialize as selector zero");
        }
        WasmCompilationResult compiled = WasmModuleCompiler.Compile(module, new(true, 4));
        Check(compiled.Succeeded && compiled.CooperativeSafepointAttestation is { Verified: true }, "host dispatch must compile with cancellation proof");
        Check(compiled.Bytes.SequenceEqual(WasmModuleCompiler.Compile(module, new(true, 4)).Bytes), "dispatch codegen must be deterministic");
        GuestModule WithoutDispatch() => module with { FramedExports = module.FramedExports.Select(export => export == route
            ? export with { HostDispatchTargets = null } : export).ToArray() };
        var direct = WasmModuleCompiler.Compile(WithoutDispatch(), new(true, 4));
        Check(direct.Succeeded && compiled.CooperativeSafepointAttestation!.RecursiveFunctionCount
            > direct.CooperativeSafepointAttestation!.RecursiveFunctionCount,
            "recursion reachable only through the selected override must enter the cancellation proof");
        JsonObject Metadata(byte[] bytes) => JsonNode.Parse(WasmArtifactInspector.Inspect(bytes).CustomSections
            .Single(section => section.Name == GuestCallFrameLayout.HostSectionName).PayloadText)!.AsObject();
        var metadata = Metadata(compiled.Bytes);
        var rows = metadata["exports"]!.AsArray();
        var dynamicRow = rows.Single(row => row!["name"]!.GetValue<string>() == route.Name)!;
        Check(metadata["schema_version"]!.GetValue<int>() == 2 && dynamicRow["dispatch_targets"]!.AsArray().Count == 2,
            "dynamic metadata must use version 2 with all targets");
        Check(dynamicRow["dispatch_targets"]![1]!["selector"]!.GetValue<uint>() == uint.MaxValue
            && dynamicRow["dispatch_targets"]![1]!["export_name"]!.GetValue<string>() == "frame:override",
            "opaque selectors and export identity must survive codegen");
        Check(rows.Where(row => row!["name"]!.GetValue<string>() != route.Name)
            .All(row => row!.AsObject().Count == 7 && !row.AsObject().ContainsKey("dispatch_targets")),
            "direct route metadata must retain its exact original shape");
        Check(Metadata(direct.Bytes)["schema_version"]!.GetValue<int>() == 1, "direct-only module must retain host metadata version 1");
        foreach (var (version, ir) in new[] { (9, "1.8"), (10, "1.9") })
            Check(GuestModuleValidator.Validate(WithoutDispatch() with { SchemaVersion = version, IrVersion = ir }).Succeeded,
                "old direct contracts must remain readable");

        GuestModule Change(GuestFramedExport changed) => module with {
            FramedExports = module.FramedExports.Select(export => export == route ? changed : export).ToArray() };
        var invalid = new List<GuestModule> {
            module with { SchemaVersion = 9, IrVersion = "1.8" },
            Change(route with { HostImportId = null }),
            Change(route with { HostDispatchTargets = Array.Empty<GuestHostDispatchTarget>() }),
            Change(route with { HostDispatchTargets = new GuestHostDispatchTarget[] { null! } }),
            Change(route with { HostDispatchTargets = new[] { new GuestHostDispatchTarget(0, null!) } }),
            Change(route with { HostDispatchTargets = new[] { new GuestHostDispatchTarget(0, "absent") } }),
            Change(route with { HostDispatchTargets = new[] { new GuestHostDispatchTarget(0, route.Name) } }),
            Change(route with { HostDispatchTargets = new[] { new GuestHostDispatchTarget(0, "frame:make") } }),
            Change(route with { HostDispatchTargets = route.HostDispatchTargets!.Reverse().ToArray() }),
            Change(route with { HostDispatchTargets = new[] { route.HostDispatchTargets![0], route.HostDispatchTargets[0] } }),
            Change(route with { HostDispatchTargets = Enumerable.Range(0, 4097).Select(i => new GuestHostDispatchTarget((uint)i, "frame:wide")).ToArray() }),
            module with { FramedExports = module.FramedExports.Select(export => export.Name == "frame:wide"
                ? export with { HostImportId = null } : export).ToArray() },
            module with { FramedExports = module.FramedExports.Select(export => export.Name == "frame:wide"
                ? export with { HostDispatchTargets = new[] { new GuestHostDispatchTarget(1, "frame:override") } } : export).ToArray() },
            module with { Functions = module.Functions.Select(function => function == stub ? function with {
                Parameters = function.Parameters.Select((parameter, i) => i == 0 ? parameter with { TypeId = "wide_i64" } : parameter).ToArray() } : function).ToArray() },
        };
        var many = Enumerable.Range(0, 4096).Select(i => new GuestHostDispatchTarget((uint)i, "frame:wide")).ToArray();
        invalid.Add(module with { FramedExports = module.FramedExports.Concat(Enumerable.Range(0, 17)
            .Select(i => route with { Name = "frame:many:" + i, HostDispatchTargets = many })).ToArray() });
        foreach (GuestModule candidate in invalid)
            Check(!GuestModuleValidator.Validate(candidate).Succeeded && !WasmModuleCompiler.Compile(candidate).Succeeded,
                "malformed host dispatch must be rejected before codegen");
        return passed;
    }
}
