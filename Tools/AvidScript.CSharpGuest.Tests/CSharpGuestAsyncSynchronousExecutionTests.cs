using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text.Json;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestAsyncSynchronousExecutionTests
{
    public static int Run()
    {
        int count = 0;
        void Check(bool valid, string reason) { if (!valid) throw new InvalidOperationException(reason); count++; }
        List<object> fixtures = new();
        string? directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_ASYNC_SYNCHRONOUS_FIXTURE_DIR");
        foreach (var scenario in new[] {
            ("success", "int value = await Read(3); return Sync(value);", 3, 0),
            ("before-await", "try { Sync(-1); Trace = 9; int value = await Read(3); return value; } catch (ArgumentException) { return 11; } finally { Trace++; }", 11, 1),
            ("after-await", "try { int value = await Read(3); return Sync(-value); } catch (ArgumentException) { return 12; } finally { Trace++; }", 12, 1),
            ("catch-return", "try { try { int value = await Read(-1); return value; } catch (InvalidOperationException) { Trace = Trace * 10 + 1; return Sync(-1); } finally { Trace = Trace * 10 + 2; } } catch (ArgumentException) { return 13; }", 13, 12),
            ("finally-return", "try { try { int value = await Read(3); return value; } finally { Trace = Trace * 10 + 1; Sync(-1); Trace = 9; } } catch (ArgumentException) { return 14; } finally { Trace = Trace * 10 + 2; }", 14, 12),
            ("finally-fault", "try { try { int value = await Read(-1); return value; } finally { Trace = Trace * 10 + 1; Sync(-1); } } catch (InvalidOperationException) { return 0; } catch (ArgumentException) { return 15; } finally { Trace = Trace * 10 + 2; }", 15, 12),
            ("rethrow", "try { try { int value = await Read(3); return Sync(-value); } catch (ArgumentException) { Trace = Trace * 10 + 1; throw; } finally { Trace = Trace * 10 + 2; } } catch (ArgumentException) { return 16; } finally { Trace = Trace * 10 + 3; }", 16, 123),
            ("argument-stop", "try { int value = await Read(3); return Pair(Sync(-value), Mark(9)); } catch (ArgumentException) { Trace = Trace * 10 + 2; return 17; } finally { Trace = Trace * 10 + 5; }", 17, 25),
            ("argument-order", "try { int value = await Read(3); return Pair(Mark(1), Sync(-value)); } catch (ArgumentException) { Trace = Trace * 10 + 2; return 18; } finally { Trace = Trace * 10 + 5; }", 18, 325),
            ("condition", "try { int value = await Read(3); if (Sync(-value) > 0) Trace = 9; return 0; } catch (ArgumentException) { return 19; } finally { Trace++; }", 19, 1),
            ("aliases", "Task<int> pending = Read(3); Task<int> alias = pending; try { int value = await pending; Sync(-value); } catch (ArgumentException) { Trace++; } int result = await alias; return result;", 3, 1),
            ("producer-reassignment", "Task<int> pending = Read(3); try { pending = Read(Sync(-1)); } catch (ArgumentException) { Trace++; } int result = await pending; return result;", 3, 1),
            ("root-propagation", "try { int value = await Failing(); return value; } catch (ArgumentException) { Trace++; return 20; }", 20, 1),
            ("loop-owner", "Task<int> pending = Read(1); try { for (int i = 0; i < 2; i++) { Task<int> local = Read(3); pending = local; int value = await local; Sync(-value); } } catch (ArgumentException) { Trace++; } int result = await pending; return result;", 3, 1),
        })
        foreach (bool deferred in new[] { false, true })
            Compile(scenario.Item1 + (deferred ? "-deferred" : "-ready"), scenario.Item2, deferred, false, scenario.Item3, scenario.Item4);

        foreach (var scenario in new[] {
            ("cancel-catch", "try { try { int value = await Read(3); return value; } catch (OperationCanceledException) { Trace = Trace * 10 + 1; return Sync(-1); } finally { Trace = Trace * 10 + 2; } } catch (ArgumentException) { return 21; } finally { Trace = Trace * 10 + 3; }", 21, 123),
            ("cancel-finally", "try { try { int value = await Read(3); return value; } finally { Trace = Trace * 10 + 1; Sync(-1); } } catch (OperationCanceledException) { return 0; } catch (ArgumentException) { return 22; } finally { Trace = Trace * 10 + 2; }", 22, 12),
            ("cancel-direct", "try { try { await AvidContinuations.NextTickAsync(); return 0; } catch (OperationCanceledException) { Trace = Trace * 10 + 1; return Sync(-1); } finally { Trace = Trace * 10 + 2; } } catch (ArgumentException) { return 23; } finally { Trace = Trace * 10 + 3; }", 23, 123),
        }) Compile(scenario.Item1, scenario.Item2, true, true, scenario.Item3, scenario.Item4);

        if (!string.IsNullOrWhiteSpace(directory))
            File.WriteAllText(Path.Combine(directory, "cases.json"), JsonSerializer.Serialize(fixtures, new JsonSerializerOptions { WriteIndented = true }));
        return count;

        void Compile(string name, string body, bool deferred, bool cancel, int expected, int trace)
        {
            string source = Source(body, deferred);
            Check(CSharpGuestAsyncThrowRoutingTests.Reference(source, cancel) == (expected, trace), name + " same-source .NET result/cleanup order");
            string sourceId = "Scripts/AsyncSynchronous_" + name + ".cs";
            var semantic = SemanticAnalyzer.Analyze(source, sourceId, FrontendAnalyzer.Analyze(source, sourceId).Source.Sha256,
                new[] { new SemanticReferenceSource(CSharpGuestContinuationTests.ReferenceFacade + CSharpGuestAsyncThrowRoutingTests.CancelFacade,
                    "generated://Continuations.cs", true) }, new SemanticCompilerWorkspace(),
                enableAsyncExceptionFlow: true, enableDirectAwaitCleanup: true, enableAsyncCancellationFlow: true,
                enableAsyncSynchronousExceptions: true);
            Check(semantic.SchemaVersion == 50 && SemanticAsyncInvocationValidator.IsValid(semantic),
                name + " source: " + string.Join(" | ", semantic.Diagnostics.Select(item => item.Code + ":" + item.Message)));
            if (!string.IsNullOrWhiteSpace(directory))
            {
                Directory.CreateDirectory(directory);
                File.WriteAllText(Path.Combine(directory, name + ".cs"), source);
                File.WriteAllBytes(Path.Combine(directory, name + ".semantic.json"), SemanticSerializer.Serialize(semantic));
            }
            string hash = Convert.ToHexString(SHA256.HashData(SemanticSerializer.Serialize(semantic))).ToLowerInvariant();
            bool lowered = CSharpLanguageErrorCompiler.TryLower(semantic, hash, out var compilation, out string? error);
            Check(lowered && compilation is not null, name + " lowering: " + error);
            var module = compilation!.Module;
            Check(module.SchemaVersion == 29 && module.IrVersion == "1.28" && module.AsyncSynchronousExceptions!.Sites.Count > 0,
                name + " source exception routes must be published with their own version");
            if (name == "loop-owner-deferred") CheckRejectedMutations(module, Check);
            var wasm = WasmModuleCompiler.Compile(module);
            Check(wasm.Succeeded, name + " WASM: " + string.Join(" | ", wasm.Diagnostics.Select(item => item.Message)));
            byte[] json = GuestIrSerializer.Serialize(module);
            var restored = GuestIrSerializer.Deserialize(json);
            Check(json.SequenceEqual(GuestIrSerializer.Serialize(restored)) && wasm.Bytes.SequenceEqual(WasmModuleCompiler.Compile(restored).Bytes),
                name + " deterministic round trip");
            if (string.IsNullOrWhiteSpace(directory)) return;
            File.WriteAllBytes(Path.Combine(directory, name + ".wasm"), wasm.Bytes);
            File.WriteAllBytes(Path.Combine(directory, name + ".guest-ir.json"), json);
            fixtures.Add(new { name, moduleId = module.ModuleId, cancel, expected, trace,
                resultOffset = module.MemoryLayout.StateSlots.Single(slot => slot.GlobalId.Contains(".Result:", StringComparison.Ordinal)).Offset,
                traceOffset = module.MemoryLayout.StateSlots.Single(slot => slot.GlobalId.Contains(".Trace:", StringComparison.Ordinal)).Offset });
        }
    }

    private static void CheckRejectedMutations(GuestModule valid, Action<bool, string> check)
    {
        var plan = valid.AsyncSynchronousExceptions!;
        var site = plan.Sites.First(item => item.MethodFunctionId.Contains(".Run(", StringComparison.Ordinal));
        string marker = site.CallBlockId + ":synchronous_exception";
        var function = valid.Functions.Single(item => item.Id == site.FunctionId);
        var fault = function.Blocks.Single(item => item.Id == marker + ":task_created");
        string task = fault.Instructions[3].OperandIds[0];
        void Reject(GuestModule candidate, string reason)
        {
            check(!GuestModuleValidator.Validate(candidate).Succeeded, reason + " must fail IR validation");
            check(!WasmModuleCompiler.Compile(candidate).Succeeded, reason + " must not produce WASM");
        }
        GuestModule Replace(GuestAsyncSynchronousExceptionSite changed) => valid with {
            AsyncSynchronousExceptions = plan with { Sites = plan.Sites.Select(item => item == site ? changed : item).ToArray() } };
        GuestModule Rewrite(string id, Func<GuestBasicBlock, GuestBasicBlock> change)
        {
            if (!function.Blocks.Any(block => block.Id == id)) throw new InvalidOperationException("Missing mutation block: " + id);
            return valid with { Functions = valid.Functions.Select(item => item.Id != function.Id ? item : item with {
                Blocks = item.Blocks.Select(block => block.Id == id ? change(block) : block).ToArray() }).ToArray() };
        }
        Reject(valid with { SchemaVersion = 26, IrVersion = "1.25" }, "downgraded IR");
        Reject(valid with { IrVersion = "1.27" }, "mismatched IR version");
        Reject(valid with { Provenance = valid.Provenance with { SemanticSchemaVersion = 46, SemanticVersion = "1.55" } }, "old source identity");
        Reject(valid with { AsyncSynchronousExceptions = null }, "missing routes");
        Reject(valid with { AsyncSynchronousExceptions = plan with { Sites = plan.Sites.Where(item => item != site).ToArray() } }, "unlisted transfer");
        Reject(valid with { AsyncSynchronousExceptions = plan with { Sites = plan.Sites.Concat(new[] { site }).ToArray() } }, "duplicate transfer");
        Reject(valid with { TaskLocalLifetimes = null }, "missing Task ownership");
        Reject(Replace(site with { CallInstructionIndex = site.CallInstructionIndex + 1 }), "wrong call index");
        Reject(Replace(site with { OwnerLocalId = site.TypeLocalId }), "wrong owner slot");
        Reject(Replace(site with { TargetBlockId = site.CallBlockId }), "wrong target metadata");
        Reject(Rewrite(site.CallBlockId, block => block with { Terminator = block.Terminator with {
            TargetBlockId = block.Terminator.FalseTargetBlockId, FalseTargetBlockId = block.Terminator.TargetBlockId } }), "reversed status paths");
        Reject(Rewrite(marker, block => block with { Terminator = block.Terminator with { FalseTargetBlockId = block.Terminator.TargetBlockId } }), "unchecked Task allocation");
        Reject(Rewrite(fault.Id, block => block with { Instructions = block.Instructions.Select((item, index) => index == 1
            ? item with { TargetId = "field:error_type" } : item).ToArray() }), "wrong outcome source token");
        Reject(Rewrite(fault.Id, block => block with { Terminator = block.Terminator with { FalseTargetBlockId = block.Terminator.TargetBlockId } }), "unchecked fault transfer");
        Reject(Rewrite(marker + ":fault_rejected", block => block with { Instructions = Array.Empty<GuestInstruction>() }), "unreleased rejected Task");
        Reject(Rewrite(marker + ":fault_acquired:release_error_owner", block => block with {
            Instructions = block.Instructions.Where(item => item.Op != "call").ToArray() }), "missing old owner release");
        foreach (string slot in new[] { site.OwnerLocalId, site.TypeLocalId })
            Reject(Rewrite(marker + ":fault_acquired:owner_released", block => block with {
                Instructions = block.Instructions.Where(item => item.TargetId != slot).ToArray() }), "uncleared owner slot " + slot);
        Reject(Rewrite(marker + ":fault_acquired:release_error_owner", block => block with { Instructions = block.Instructions.Append(
            new GuestInstruction("local_store", null, new[] { task }, site.OwnerLocalId, null, null)).ToArray() }), "owner published during old-owner release");
        Reject(Rewrite(marker + ":publish", block => block with { Instructions = block.Instructions.Skip(1).ToArray() }), "missing owner publication");
        Reject(Rewrite(marker + ":publish", block => block with { Terminator = block.Terminator with { TargetBlockId = site.CallBlockId } }), "wrong executable target");
        Reject(valid with { Functions = valid.Functions.Select(item => item.Id == function.Id
            ? item with { EntryBlockId = fault.Id } : item).ToArray() }, "entry bypasses outcome status and allocation");
        string success = function.Blocks.Single(block => block.Id == site.CallBlockId).Terminator.FalseTargetBlockId!;
        Reject(Rewrite(success, block => block with { Instructions = fault.Instructions.Take(1).Concat(block.Instructions).ToArray() }), "loop success reads an error payload");
    }

    private static string Source(string body, bool deferred) =>
        "using AvidScript; using System; using System.Runtime.InteropServices; using System.Threading.Tasks; public static class Script { "
        + "public static int Result; public static int Trace; "
        + "[UnmanagedCallersOnly(EntryPoint = \"avid_on_begin_play\")] public static async void BeginPlay() { Result = await Run(); } "
        + "public static int Sync(int value) { if (value < 0) throw new ArgumentException(); return value; } "
        + "public static int Mark(int value) { Trace = Trace * 10 + 3; return value; } "
        + "public static int Pair(int first, int second) { Trace = Trace * 10 + 4; return first + second; } "
        + "public static Task<int> Make(int value) { Sync(value); return Read(value); } "
        + "public static async Task<int> Read(int value) { " + (deferred ? "await AvidContinuations.NextTickAsync(); " : "")
        + "if (value < 0) throw new InvalidOperationException(); return value; } "
        + "public static async Task<int> Failing() { int value = await Read(3); return Sync(-value); } "
        + "public static async Task<int> Run() { " + body + " } }";
}
