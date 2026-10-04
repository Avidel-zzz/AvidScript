using System;
using System.IO;
using System.Linq;
using System.Text;
using AvidScript.GuestIr;

internal static class GuestTaskValueCatalogTests
{
    private static void Check(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }
    private static void Reject(Action action) { try { action(); } catch (ArgumentException) { return; } throw new InvalidOperationException("Invalid task catalog accepted."); }
    private static GuestModule ScalarModule()
    {
        GuestModule original = GuestModuleValidationTests.CreateMinimalModule();
        return original with { ModuleId = "catalog-fixture", Types = original.Types.Append(new GuestType(
            "type:float32", "scalar", "f32", Array.Empty<GuestField>(), null, null, 4, 4)).ToArray() };
    }
    private static GuestModule ManagedModule()
    {
        GuestModule scalar = ScalarModule();
        GuestType[] declarations = scalar.Types.Concat(new GuestType[] {
            new("r:erased", "managed_ref", "i64", Array.Empty<GuestField>(), null, null, 8, 8),
            new("r:node", "managed_ref", "i64", Array.Empty<GuestField>(), "p:node", null, 8, 8),
            new("p:node", "struct", "memory", new[] { new GuestField("p:h", "Health", "type:int32", 0), new GuestField("p:n", "Next", "r:node", 0) }, null, null, 0, 1),
            new("v:result", "struct", "memory", new[] { new GuestField("v:f", "Damage", "type:float32", 0), new GuestField("v:n", "Node", "r:node", 0), new GuestField("v:a", "Alias", "r:node", 0) }, null, null, 0, 1)
        }).ToArray();
        GuestTypeLayoutResult layouts = GuestDataLayout.ComputeTypes(declarations);
        Check(layouts.Succeeded, "Catalog fixture layouts failed.");
        return scalar with { SchemaVersion = 14, IrVersion = "1.13", Types = layouts.Types,
            Imports = new[] { new GuestImport("heap", "avidscript", GuestManagedHeap.ImportName,
                Enumerable.Repeat("type:int32", 4).ToArray(), "type:int32") } };
    }
    public static int Run()
    {
        GuestModule scalar = ScalarModule();
        byte[] absent = GuestIrSerializer.Serialize(scalar);
        Check(!Encoding.UTF8.GetString(absent).Contains("task_value_catalog", StringComparison.Ordinal), "Absent metadata changed canonical JSON.");
        GuestTaskValueCatalogPlan plan = GuestTaskValueCatalog.Build(scalar.Types, new[] { "type:int32", "type:float32" });
        GuestModule module = scalar with { TaskValueCatalog = plan };
        Check(plan.Results[0].TypeId == "type:float32" && plan.Results[1].Ordinal == 2, "Catalog ordering is not deterministic.");
        Check(GuestModuleValidator.Validate(module).Succeeded, "Original catalog module failed validation.");
        Check(GuestTaskValueCatalog.Encode(module).SequenceEqual(GuestTaskValueCatalog.Encode(GuestIrSerializer.Deserialize(GuestIrSerializer.Serialize(module)))), "Catalog JSON/wire round trip changed bytes.");
        foreach (GuestTaskValueCatalogPlan invalid in new[] { plan with { Version = 2 }, plan with { Results = null! },
            plan with { Results = Array.Empty<GuestTaskValueCatalogEntry>() }, plan with { Results = new[] { plan.Results[1], plan.Results[0] } },
            plan with { Results = new[] { plan.Results[0] with { Ordinal = 2 } } }, plan with { Results = new[] { plan.Results[0] with { ValuePlan = null! } } },
            plan with { Results = new GuestTaskValueCatalogEntry[] { null! } } })
        {
            GuestValidationResult invalidResult = GuestModuleValidator.Validate(module with { TaskValueCatalog = invalid });
            Check(!invalidResult.Succeeded && invalidResult.Diagnostics.Any(d => d.Code == "ASIR1081"), "Malformed metadata did not fail closed.");
        }
        byte[] forged = plan.Results[0].ValuePlan.ToArray(); forged[^1] ^= 1;
        Check(!GuestModuleValidator.Validate(module with { TaskValueCatalog = plan with {
            Results = new[] { plan.Results[0] with { ValuePlan = forged }, plan.Results[1] } } }).Succeeded, "Forged value packet accepted.");
        GuestModule changedType = module with { Types = scalar.Types.Select(type => type.Id == "type:float32"
            ? type with { Storage = "i32" } : type).ToArray() };
        Check(!GuestModuleValidator.Validate(changedType).Succeeded, "Stale plan survived original declaration change.");
        Reject(() => GuestTaskValueCatalog.Build(scalar.Types, Array.Empty<string>()));
        Reject(() => GuestTaskValueCatalog.Build(scalar.Types, new[] { "type:int32", "type:int32" }));
        Reject(() => GuestTaskValueCatalog.Build(scalar.Types, Enumerable.Repeat("type:int32", GuestTaskValueCatalog.MaxResults + 1)));
        Reject(() => GuestTaskValueCatalog.Encode(module with { ModuleId = new string('x', 1025) }));
        Reject(() => GuestTaskValueCatalog.Encode(module with { ModuleId = "bad\0module" }));
        Reject(() => GuestTaskValueCatalog.Encode(module with { Provenance = module.Provenance with { SourceSha256 = new string('0', 64) } }));
        GuestModule managed = ManagedModule();
        managed = managed with { TaskValueCatalog = GuestTaskValueCatalog.Build(managed.Types, new[] { "v:result", "type:float32" }) };
        Check(GuestModuleValidator.Validate(managed).Succeeded, "Managed catalog did not validate against original types.");
        byte[] before = GuestTaskValueCatalog.Encode(managed);
        Check(before.SequenceEqual(GuestTaskValueCatalog.Encode(managed)), "Managed catalog encoding is not deterministic.");
        string header = File.ReadAllText(Path.Combine(Directory.GetCurrentDirectory(), "Source/AvidScriptCore/Public/AvidScriptTaskValueCatalogAbi.h"));
        Check(header.Contains("0x31435654", StringComparison.Ordinal) && header.Contains("MaxResults = 128", StringComparison.Ordinal), "Native catalog ABI constants drifted.");
        string? directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_TASK_VALUE_CATALOG_DIR");
        if (!string.IsNullOrEmpty(directory))
        {
            Directory.CreateDirectory(directory);
            File.WriteAllBytes(Path.Combine(directory, "scalar.catalog"), GuestTaskValueCatalog.Encode(module));
            File.WriteAllBytes(Path.Combine(directory, "managed.catalog"), before);
            Console.WriteLine("AvidScript.TaskValueCatalog.CompilerWire: 2/2 written");
        }
        return 6;
    }
}
