using System;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestAsyncCancellationTests
{
    public static int Run()
    {
        const string sourceId = "Fixtures/Phase66/AsyncCancellationFlow.cs";
        string source = File.ReadAllText(sourceId) + "\n"
            + File.ReadAllText("Fixtures/Phase66/AsyncCancellationFlow.Guest.cs");
        const string additionalFacade = """

            internal static class CancelResumeHost
            {
                [System.Runtime.InteropServices.DllImport("avidscript",
                    EntryPoint = "avid_continuation_delay_cancel_resume_v1")]
                internal static extern long Delay(float seconds, int callbackId);
            }
            """;
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        SemanticDocument semantic = SemanticAnalyzer.Analyze(source, sourceId,
            frontend.Source.Sha256, new[] { new SemanticReferenceSource(
                CSharpGuestContinuationTests.ReferenceFacade + additionalFacade,
                "generated://AvidScript.Continuations.generated.cs", true) },
            new SemanticCompilerWorkspace(), enableAsyncExceptionFlow: true,
            enableDirectAwaitCleanup: true, enableAsyncCancellationFlow: true);
        Check(semantic.Succeeded && semantic.SchemaVersion == 44,
            "Semantic cancellation fixture: " + string.Join(" | ", semantic.Diagnostics.Select(item => item.Message)));
        string hash = Convert.ToHexString(SHA256.HashData(SemanticSerializer.Serialize(semantic))).ToLowerInvariant();
        Check(!CSharpGuestLowerer.Lower(semantic, hash).Succeeded, "Cancellation must remain opt-in.");
        CSharpGuestLoweringResult lowered = CSharpGuestLowerer.Lower(semantic, hash, enableAsyncLanguageErrors: true);
        Check(lowered.Succeeded && lowered.Module is not null,
            "Cancellation lowering: " + string.Join(" | ", lowered.Diagnostics.Select(item => item.Code + ": " + item.Message)));
        GuestModule module = lowered.Module!;
        Check(module.SchemaVersion == 24 && module.IrVersion == "1.23"
            && module.DirectAwaitRoutes is { Count: 3 } && module.AsyncExceptionRoutes is { Count: 2 }
            && module.AsyncExceptionTransfers is { Count: > 0 }, "Cancellation routes and transfers must be explicit.");
        int checks = 6;
        Reject(module with { AsyncExceptionTransfers = null }, "ASIR1033", "missing exception ownership metadata");
        Reject(module with { DirectAwaitRoutes = module.DirectAwaitRoutes!.Skip(1).ToArray() },
            "ASIR1031", "omitted direct cancellation producer");
        Reject(module with { AsyncExceptionRoutes = module.AsyncExceptionRoutes!.Skip(1).ToArray() },
            "ASIR1030", "omitted protected Task await");
        GuestDirectAwaitRoute direct = module.DirectAwaitRoutes![0];
        Reject(module with { DirectAwaitRoutes = module.DirectAwaitRoutes.Select(route => route == direct
            ? route with { Cancellation = null } : route).ToArray() }, "ASIR1031", "missing typed Timer owner");
        Reject(module with { DirectAwaitRoutes = module.DirectAwaitRoutes.Select(route => route == direct
            ? route with { Cancellation = route.Cancellation! with { TypeToken = int.MaxValue } } : route).ToArray() },
            "ASIR1031", "forged cancellation type token");
        Reject(module with { DirectAwaitRoutes = module.DirectAwaitRoutes.Select(route => route == direct
            ? route with { CancellationTargetBlockId = route.NormalTargetBlockId } : route).ToArray() },
            "ASIR1031", "cancellation redirected to normal code");
        GuestAsyncExceptionRoute task = module.AsyncExceptionRoutes![0];
        Reject(module with { AsyncExceptionRoutes = module.AsyncExceptionRoutes.Select(route => route == task
            ? route with { CancellationTargetBlockId = route.NormalTargetBlockId } : route).ToArray() },
            "ASIR1030", "Task cancellation bypasses source catch dispatch");
        GuestAsyncExceptionTransfer consume = module.AsyncExceptionTransfers!.First(transfer => transfer.Kind == "end_catch");
        string consumed = consume.BlockId + ":end_catch:owner_released";
        Reject(Rewrite(consumed, block => block with { Instructions = block.Instructions.Where(instruction =>
            instruction.Op != "local_store" || instruction.TargetId != consume.OwnerLocalId).ToArray() }),
            "ASIR1033", "handled cancellation keeps a released owner");
        Reject(Rewrite(consumed, block => block with { Instructions = block.Instructions.Where(instruction =>
            instruction.Op != "local_store" || instruction.TargetId != consume.TypeLocalId).ToArray() }),
            "ASIR1033", "handled cancellation keeps a stale type");
        GuestAsyncExceptionTransfer rethrow = module.AsyncExceptionTransfers!.First(transfer => transfer.Kind == "rethrow");
        Reject(Rewrite(rethrow.BlockId + ":rethrow", block => block with
        { Terminator = block.Terminator with { TargetBlockId = rethrow.BlockId } }),
            "ASIR1033", "rethrow loops back into the same handler");
        GuestAsyncExceptionTransfer propagate = module.AsyncExceptionTransfers!.First(transfer => transfer.Kind == "propagate_exception");
        Reject(Rewrite(propagate.BlockId + ":propagate_exception", block => block with
        {
            Instructions = block.Instructions.Select(instruction => instruction.Op == "call"
                && instruction.TargetId == "import:$async:task_propagate_failure_v1"
                ? instruction with { OperandIds = instruction.OperandIds.Reverse().ToArray() } : instruction).ToArray(),
        }), "ASIR1033", "reversed Task failure propagation");
        Reject(module with { SchemaVersion = 23, IrVersion = "1.22" }, "ASIR1033", "downgraded cancellation transfers");
        byte[] json = GuestIrSerializer.Serialize(module);
        Check(json.SequenceEqual(GuestIrSerializer.Serialize(GuestIrSerializer.Deserialize(json))),
            "Cancellation IR must round trip canonically.");
        WasmCompilationResult compiled = WasmModuleCompiler.Compile(module);
        Check(compiled.Succeeded, "Cancellation codegen: " + string.Join(" | ", compiled.Diagnostics.Select(item => item.Message)));
        string? directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_CSHARP_CANCELLATION_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(directory))
        {
            Directory.CreateDirectory(directory);
            File.WriteAllBytes(Path.Combine(directory, "cancellation.semantic.json"), SemanticSerializer.Serialize(semantic));
            File.WriteAllBytes(Path.Combine(directory, "cancellation.guest-ir.json"), json);
            File.WriteAllBytes(Path.Combine(directory, "cancellation.wasm"), compiled.Bytes);
        }
        return checks;

        void Reject(GuestModule candidate, string code, string reason)
        {
            Check(GuestModuleValidator.Validate(candidate).Diagnostics.Any(item => item.Code == code),
                "Cancellation IR must reject " + reason);
            ++checks;
        }

        GuestModule Rewrite(string blockId, Func<GuestBasicBlock, GuestBasicBlock> update)
        {
            Check(module.Functions.Any(function => function.Blocks.Any(block => block.Id == blockId)),
                "Missing mutation target: " + blockId);
            return module with { Functions = module.Functions.Select(function => function with
            { Blocks = function.Blocks.Select(block => block.Id == blockId ? update(block) : block).ToArray() }).ToArray() };
        }
    }

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}
