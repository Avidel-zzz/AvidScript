using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json.Nodes;
using AvidScript.GuestIr;

internal static class GuestTaskErrorTransferTests
{
    public static int Run()
    {
        int count = 0;
        void Check(bool condition, string message)
        {
            if (!condition) throw new InvalidOperationException("Task error transfer: " + message);
            count++;
        }
        foreach (bool cached in new[] { false, true })
        {
            GuestModule module = GuestTaskErrorTransferFixture.Create(cached);
            var valid = GuestModuleValidator.Validate(module);
            Check(valid.Succeeded, string.Join(" | ", valid.Diagnostics.Select(item => item.Message)));
            byte[] json = GuestIrSerializer.Serialize(module);
            Check(json.SequenceEqual(GuestIrSerializer.Serialize(GuestIrSerializer.Deserialize(json))), "canonical round trip");
            GuestTaskErrorTransferPlan plan = module.TaskErrorTransfers!;
            var site = plan.Sites[0];
            GuestModule Site(GuestTaskErrorTransferSite value) => module with { TaskErrorTransfers = plan with { Sites = new[] { value } } };
            GuestModule Block(string id, Func<GuestBasicBlock, GuestBasicBlock> mutate) => module with
            {
                Functions = module.Functions.Select(function => function.Id == "transfer" ? function with
                {
                    Blocks = function.Blocks.Select(block => block.Id == id ? mutate(block) : block).ToArray(),
                } : function).ToArray(),
            };
            GuestModule Instruction(string block, int index, Func<GuestInstruction, GuestInstruction> mutate) =>
                Block(block, value => value with { Instructions = value.Instructions.Select((item, i) => i == index ? mutate(item) : item).ToArray() });
            var invalid = new List<(string Name, GuestModule Module)>
            {
                ("missing plan", module with { TaskErrorTransfers = null }),
                ("downgrade with metadata", module with { SchemaVersion = 24, IrVersion = "1.23" }),
                ("downgrade without metadata", module with { SchemaVersion = 24, IrVersion = "1.23", TaskErrorTransfers = null }),
                ("wrong outer pair", module with { IrVersion = "1.26" }),
                ("future outer", module with { SchemaVersion = 29, IrVersion = "1.28" }),
                ("wrong base pair", module with { TaskErrorTransfers = plan with { BaseIrVersion = "1.22" } }),
                ("old base", module with { TaskErrorTransfers = plan with { BaseSchemaVersion = 19, BaseIrVersion = "1.18" } }),
                ("recursive base", module with { TaskErrorTransfers = plan with { BaseSchemaVersion = 28, BaseIrVersion = "1.27" } }),
                ("base provenance", module with { Provenance = module.Provenance with { SemanticVersion = "1.52" } }),
                ("missing catalog", module with { LanguageErrorCatalog = null }),
                ("missing outcomes", module with { LanguageOutcomeTypes = null }),
                ("empty outcomes", module with { LanguageOutcomeTypes = Array.Empty<GuestLanguageOutcomeType>() }),
                ("empty sites", module with { TaskErrorTransfers = plan with { Sites = Array.Empty<GuestTaskErrorTransferSite>() } }),
                ("duplicate sites", module with { TaskErrorTransfers = plan with { Sites = new[] { site, site } } }),
                ("too many sites", module with { TaskErrorTransfers = plan with { Sites = Enumerable.Repeat(site, 4097).ToArray() } }),
                ("null sites", module with { TaskErrorTransfers = plan with { Sites = null! } }),
                ("null site", module with { TaskErrorTransfers = plan with { Sites = new GuestTaskErrorTransferSite[] { null! } } }),
                ("null base", module with { TaskErrorTransfers = plan with { BaseIrVersion = null! } }),
                ("null function", Site(site with { FunctionId = null! })),
                ("missing function", Site(site with { FunctionId = "absent" })),
                ("missing block", Site(site with { ErrorBlockId = "absent" })),
                ("same block", Site(site with { ErrorBlockId = "entry" })),
                ("negative call index", Site(site with { CallInstructionIndex = -1 })),
                ("overflow call index", Site(site with { CallInstructionIndex = int.MaxValue })),
                ("wrong call index", Site(site with { CallInstructionIndex = 1 })),
                ("negative fault index", Site(site with { FaultInstructionIndex = -1 })),
                ("out of bounds fault index", Site(site with { FaultInstructionIndex = 5 })),
                ("wrong fault index", Site(site with { FaultInstructionIndex = 3 })),
                ("status bypass", Block("entry", block => block with { Terminator = new("branch", null, "error", null, null) })),
                ("status inversion", Block("entry", block => block with { Terminator = new("branch_if", "status", "success", "error", null) })),
                ("extra predecessor", Block("success", block => block with { Terminator = new("branch", null, "error", null, null) })),
                ("nonstatus field", Instruction("entry", 1, op => op with { TargetId = "field:error_type" })),
                ("wrong callee", Instruction("entry", 0, op => op with { TargetId = "fault" })),
                ("wrong outcome", Instruction("error", 1, op => op with { OperandIds = new[] { "status" } })),
                ("wrong source", Instruction("error", 2, op => op with { TargetId = "field:error_type" })),
                ("wrong root", Instruction("error", 4, op => op with { OperandIds = new[] { "task", "type", "source", "task" } })),
                ("unlisted second fault", Block("error", block => block with { Instructions = block.Instructions.Append(block.Instructions[4]).ToArray() })),
                ("overwritten outcome", Instruction("error", 0, op => GuestStaticStorageFixture.Op("stack_alloc", "outcome"))),
                ("aliased outcome", Instruction("error", 0, op => GuestStaticStorageFixture.Op("copy", "outcome", new[] { "outcome" }))),
                ("mutated outcome", Instruction("error", 0, op => GuestStaticStorageFixture.Op("field_store", operands: new[] { "outcome", "status" }, target: "field:status"))),
                ("overwritten status", Instruction("error", 0, op => GuestStaticStorageFixture.Op("field_load", "status", new[] { "outcome" }, "field:error_type"))),
                ("unpaired fault import", module with { Imports = module.Imports.Where(item => item.Id != GuestTaskLanguageErrorValidator.ImportId).ToArray() }),
            };
            if (cached) invalid.Add(("mismatched static profile", module with
            { StaticStorage = module.StaticStorage! with { BaseSchemaVersion = 20, BaseIrVersion = "1.19" } }));
            foreach (var candidate in invalid)
                Check(!GuestModuleValidator.Validate(candidate.Module).Succeeded, candidate.Name + " was accepted");
            foreach (string field in new[] { "base_schema_version", "base_ir_version", "sites", "function_id", "call_block_id", "call_instruction_index", "error_block_id", "fault_instruction_index" })
            {
                JsonObject node = JsonNode.Parse(json)!.AsObject();
                JsonObject contract = node["task_error_transfers"]!.AsObject();
                (contract.ContainsKey(field) ? contract : contract["sites"]![0]!.AsObject()).Remove(field);
                bool rejected;
                try { rejected = !GuestModuleValidator.Validate(GuestIrSerializer.Deserialize(Encoding.UTF8.GetBytes(node.ToJsonString()))).Succeeded; }
                catch (InvalidDataException) { rejected = true; }
                Check(rejected, "missing serialized " + field);
            }
        }
        byte[] legacy = GuestIrSerializer.Serialize(GuestTaskCancellationFixture.Create());
        Check(!Encoding.UTF8.GetString(legacy).Contains("task_error_transfers", StringComparison.Ordinal), "old IR gained a serialized field");
        return count;
    }
}
