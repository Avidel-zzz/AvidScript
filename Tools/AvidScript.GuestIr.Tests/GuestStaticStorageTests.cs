using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json.Nodes;
using AvidScript.GuestIr;

internal static class GuestStaticStorageTests
{
    public static int Run()
    {
        int count = 0;
        void Check(bool condition, string message)
        {
            if (!condition) throw new InvalidOperationException("Static IR: " + message);
            count++;
        }
        GuestModule module = GuestStaticStorageFixture.Create();
        GuestValidationResult valid = GuestModuleValidator.Validate(module);
        Check(valid.Succeeded, string.Join(" | ", valid.Diagnostics.Select(item => item.Message)));
        byte[] json = GuestIrSerializer.Serialize(module);
        Check(json.SequenceEqual(GuestIrSerializer.Serialize(GuestIrSerializer.Deserialize(json))), "canonical round trip");
        Check(GuestIrSerializer.Deserialize(json).StaticStorage?.Slots.Count == 3, "slot declarations survive serialization");
        GuestStaticStoragePlan plan = module.StaticStorage!;
        GuestModule With(GuestStaticStoragePlan value) => module with { StaticStorage = value };
        GuestModule Instruction(GuestInstruction value) => module with
        {
            Functions = module.Functions.Select(function => function.Id == "read" ? function with
            {
                Blocks = new[] { function.Blocks[0] with { Instructions = new[] { value }
                    .Concat(function.Blocks[0].Instructions.Skip(1)).ToArray() } },
            } : function).ToArray(),
        };
        var get = GuestStaticStorageFixture.Op(GuestStaticStorage.GetOp, "a", target: GuestStaticStorageFixture.Current);
        var invalid = new List<(string Name, GuestModule Module)>
        {
            ("older header", module with { SchemaVersion = 14, IrVersion = "1.13" }),
            ("missing plan", module with { StaticStorage = null }),
            ("wrong outer version", module with { IrVersion = "1.25" }),
            ("future outer", module with { SchemaVersion = 28, IrVersion = "1.27" }),
            ("empty slots", With(plan with { Slots = Array.Empty<GuestStaticSlot>() })),
            ("null slots", With(plan with { Slots = null! })),
            ("null slot", With(plan with { Slots = new GuestStaticSlot[] { null! } })),
            ("null ID", With(plan with { Slots = new[] { new GuestStaticSlot(null!, GuestStaticStorageFixture.R) } })),
            ("null type", With(plan with { Slots = new[] { new GuestStaticSlot("slot", null!) } })),
            ("null base version", With(plan with { BaseIrVersion = null! })),
            ("self profile", With(plan with { BaseSchemaVersion = 27, BaseIrVersion = "1.26" })),
            ("future profile", With(plan with { BaseSchemaVersion = 28, BaseIrVersion = "1.27" })),
            ("unsupported old profile", With(plan with { BaseSchemaVersion = 3, BaseIrVersion = "1.2" })),
            ("mismatched profile", With(plan with { BaseIrVersion = "1.12" })),
            ("missing task ownership", With(plan with { BaseSchemaVersion = 26, BaseIrVersion = "1.25" })),
            ("empty ID", With(plan with { Slots = new[] { new GuestStaticSlot("", GuestStaticStorageFixture.R) } })),
            ("duplicate ID", With(plan with { Slots = new[] { plan.Slots[0], plan.Slots[0] } })),
            ("type ID collision", With(plan with { Slots = new[] { new GuestStaticSlot(GuestStaticStorageFixture.R, GuestStaticStorageFixture.R) } })),
            ("function ID collision", With(plan with { Slots = new[] { new GuestStaticSlot("tick", GuestStaticStorageFixture.R) } })),
            ("scalar slot", With(plan with { Slots = new[] { plan.Slots[0] with { TypeId = GuestStaticStorageFixture.I } } })),
            ("unknown type", With(plan with { Slots = new[] { plan.Slots[0] with { TypeId = "missing" } } })),
            ("oversized table", With(plan with { Slots = Enumerable.Range(0, GuestManagedHeap.MaxStaticSlots + 1)
                .Select(index => new GuestStaticSlot("slot:" + index, GuestStaticStorageFixture.R)).ToArray() })),
            ("get missing result", Instruction(get with { ResultId = null })),
            ("get wrong type", Instruction(get with { ResultId = "x" })),
            ("get operands", Instruction(get with { OperandIds = new[] { "unused1" } })),
            ("get missing slot", Instruction(get with { TargetId = "missing" })),
            ("get no slot", Instruction(get with { TargetId = null })),
            ("get unexpected operator", Instruction(get with { OperatorKind = "add" })),
            ("get unexpected constant", Instruction(get with { Constant = new("null", null) })),
            ("set result", Instruction(get with { Op = GuestStaticStorage.SetOp, OperandIds = new[] { "a" } })),
            ("set scalar", Instruction(get with { Op = GuestStaticStorage.SetOp, ResultId = null, OperandIds = new[] { "unused1" } })),
            ("set missing value", Instruction(get with { Op = GuestStaticStorage.SetOp, ResultId = null })),
            ("erased slot bypass", Instruction(get with { TargetId = GuestStaticStorageFixture.Erased })),
            ("raw global bypass", module with { Globals = new[] { new GuestGlobal("unsafe", GuestStaticStorageFixture.R, true, new("null", null)) } }),
        };
        foreach (var candidate in invalid)
            Check(!GuestModuleValidator.Validate(candidate.Module).Succeeded, candidate.Name + " was accepted");

        foreach (string property in new[] { "base_schema_version", "base_ir_version", "slots" })
        {
            JsonObject node = JsonNode.Parse(json)!.AsObject();
            node["static_storage"]!.AsObject().Remove(property);
            bool rejected = false;
            try { rejected = !GuestModuleValidator.Validate(GuestIrSerializer.Deserialize(Encoding.UTF8.GetBytes(node.ToJsonString()))).Succeeded; }
            catch (InvalidDataException) { rejected = true; }
            Check(rejected, "missing serialized " + property);
        }
        GuestModule cancellation = GuestStaticStorageFixture.WithCancellation();
        valid = GuestModuleValidator.Validate(cancellation);
        Check(valid.Succeeded, "cancellation composition: " + string.Join(" | ", valid.Diagnostics.Select(item => item.Message)));
        Check(!GuestModuleValidator.Validate(cancellation with { LanguageErrorCatalog = null }).Succeeded, "static profile bypassed error catalog");
        Check(!GuestModuleValidator.Validate(cancellation with { Provenance = cancellation.Provenance with { SemanticVersion = "1.52" } }).Succeeded,
            "static profile bypassed Semantic pairing");
        Check(!GuestModuleValidator.Validate(cancellation with { StaticStorage = cancellation.StaticStorage! with { BaseSchemaVersion = 14, BaseIrVersion = "1.13" } }).Succeeded,
            "downgraded base profile retained cancellation capabilities");
        GuestModule legacy = GuestTaskCancellationFixture.Create();
        Check(!Encoding.UTF8.GetString(GuestIrSerializer.Serialize(legacy)).Contains("static_storage", StringComparison.Ordinal), "old IR gained serialized fields");
        return count;
    }
}
