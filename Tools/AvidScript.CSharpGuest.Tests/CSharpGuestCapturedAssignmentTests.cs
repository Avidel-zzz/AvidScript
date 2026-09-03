using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestCapturedAssignmentTests
{
    public static int Run()
    {
        ShortCircuitAssignmentsWriteOriginalStorage(useGlobal: true);
        ShortCircuitAssignmentsWriteOriginalStorage(useGlobal: false);
        RefAssignmentWritesOriginalAddress();
        RightHandSideCapturesKeepValueSnapshots();
        MergedValueCapturesDoNotAliasStorage();
        CapturedAggregateTargetFailsClosed();
        CapturedArrayTargetFailsClosed();
        return 7;
    }

    private static void ShortCircuitAssignmentsWriteOriginalStorage(bool useGlobal)
    {
        string declaration = useGlobal ? "" : "bool Ready;";
        GuestModule module = Lower($$"""
            private static bool Ready;
            [UnmanagedCallersOnly(EntryPoint = "test")]
            public static int Test()
            {
                {{declaration}}
                Ready = ReadFlag(4) != 0;
                Ready = ReadFlag(0) != 0 && ReadFlag(1) != 0
                    && ReadFlag(2) != 0 && ReadFlag(3) != 0;
                return Ready ? 1 : 0;
            }
            """);
        GuestFunction function = TestFunction(module);
        string storage = useGlobal
            ? module.Globals.Single(item => item.Id.Contains(".Ready:", StringComparison.Ordinal)).Id
            : function.Locals.Single(item => item.Id.StartsWith("value:local:", StringComparison.Ordinal)
                && item.Id.Contains(":Ready:", StringComparison.Ordinal)).Id;
        Assert(function.Blocks.SelectMany(block => block.Instructions).Count(item =>
                item.Op == (useGlobal ? "global_store" : "local_store") && item.TargetId == storage) == 2,
            "both initialization and captured assignment must write the original storage");

        // All truth-table rows, with both previous values, exercise every short-circuit edge.
        for (int mask = 0; mask < 32; ++mask)
        {
            List<int> calls = new();
            int actual = Trace(module, mask, calls);
            Assert(actual == ((mask & 15) == 15 ? 1 : 0), $"wrong short-circuit result for mask {mask}");
            int firstFalse = Enumerable.Range(0, 4).FirstOrDefault(index => (mask & (1 << index)) == 0, 3);
            Assert(calls.SequenceEqual(new[] { 4 }.Concat(Enumerable.Range(0, firstFalse + 1))),
                $"RHS call order or short circuit changed for mask {mask}");
        }
    }

    private static void RefAssignmentWritesOriginalAddress()
    {
        GuestModule module = Lower("""
            private static void Assign(ref bool target)
            {
                target = ReadFlag(0) != 0 && ReadFlag(1) != 0;
            }
            [UnmanagedCallersOnly(EntryPoint = "test")]
            public static int Test()
            {
                bool target = false;
                Assign(ref target);
                return target ? 1 : 0;
            }
            """);
        GuestFunction function = module.Functions.Single(item => item.Id.Contains(".Assign(", StringComparison.Ordinal));
        GuestInstruction store = function.Blocks.SelectMany(block => block.Instructions)
            .Single(item => item.Op == "indirect_store");
        Assert(store.OperandIds[0] == function.Parameters.Single().Id && store.TargetId == "type:bool",
            "captured ref assignment must write through the original caller address");
    }

    private static void RightHandSideCapturesKeepValueSnapshots()
    {
        foreach (bool useGlobal in new[] { true, false })
        {
            string declaration = useGlobal ? "" : "int Value;";
            GuestModule module = Lower($$"""
                private static int Value;
                [UnmanagedCallersOnly(EntryPoint = "test")]
                public static int Test()
                {
                    {{declaration}}
                    Value = 7;
                    return First(Value, ReadFlag(0) != 0 ? (Value = 9) : (Value = 11));
                }
                """);
            for (int mask = 0; mask < 2; ++mask)
            {
                Assert(Trace(module, mask, new List<int>()) == 7,
                    "a captured RHS argument must keep its old value across later writes");
            }
        }
    }

    private static void MergedValueCapturesDoNotAliasStorage()
    {
        GuestModule module = Lower("""
            private static int Left;
            private static int Right;
            [UnmanagedCallersOnly(EntryPoint = "test")]
            public static int Test()
            {
                Left = 7;
                Right = 9;
                return ReadFlag(0) != 0 ? Left : Right;
            }
            """);
        Assert(Trace(module, 0, new List<int>()) == 9 && Trace(module, 1, new List<int>()) == 7,
            "different global sources may merge into a read-only value capture");
    }

    private static void CapturedAggregateTargetFailsClosed()
    {
        SemanticDocument semantic = Analyze("""
            public struct Pair { public bool Value; }
            private static Pair State;
            [UnmanagedCallersOnly(EntryPoint = "test")]
            public static int Test()
            {
                State.Value = ReadFlag(0) != 0 && ReadFlag(1) != 0;
                return State.Value ? 1 : 0;
            }
            """);
        CSharpGuestLoweringResult lowering = CSharpGuestLowerer.Lower(semantic, new string('c', 64));
        Assert(!lowering.Succeeded && lowering.Module is null
            && lowering.Diagnostics.Any(item => item.Code == "ASCG1004"
                && item.Message.Contains("captured assignment target", StringComparison.Ordinal)),
            "an unsupported captured aggregate address must fail closed, never write a value copy");
    }

    private static void CapturedArrayTargetFailsClosed()
    {
        SemanticDocument semantic = Analyze("""
            [UnmanagedCallersOnly(EntryPoint = "test")]
            public static int Test()
            {
                bool[] values = new bool[] { false };
                values[ReadFlag(2)] = ReadFlag(0) != 0 && ReadFlag(1) != 0;
                return values[0] ? 1 : 0;
            }
            """);
        CSharpGuestLoweringResult lowering = CSharpGuestLowerer.Lower(semantic, new string('c', 64));
        Assert(!lowering.Succeeded && lowering.Module is null
            && lowering.Diagnostics.Any(item => item.Code == "ASCG1004"
                && item.Message.Contains("captured assignment target", StringComparison.Ordinal)),
            "an unsupported captured array/index address must fail closed, never write a value copy");
    }

    private static SemanticDocument Analyze(string members)
    {
        string source = $$"""
            using System.Runtime.InteropServices;
            public static class Script
            {
                [DllImport("env", EntryPoint = "read_flag")]
                private static extern int ReadFlag(int index);
                [DllImport("env", EntryPoint = "first")]
                private static extern int First(int first, int second);
                {{members}}
            }
            """;
        const string sourceId = "Scripts/CapturedAssignments.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        SemanticDocument semantic = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);
        Assert(semantic.Succeeded, string.Join(" | ", semantic.Diagnostics.Select(item => item.Message)));
        return semantic;
    }

    private static GuestModule Lower(string members)
    {
        CSharpGuestLoweringResult lowering = CSharpGuestLowerer.Lower(Analyze(members), new string('c', 64));
        Assert(lowering.Succeeded && lowering.Module is not null,
            string.Join(" | ", lowering.Diagnostics.Select(item => item.Message)));
        GuestModule module = lowering.Module!;
        Assert(GuestModuleValidator.Validate(module).Succeeded, "captured assignment Guest IR must validate");
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(module);
        Assert(wasm.Succeeded && wasm.Bytes.Length > 8, "captured assignment Guest IR must emit WASM");
        return module;
    }

    private static GuestFunction TestFunction(GuestModule module)
        => module.Functions.Single(item => item.Id.Contains(".Test(", StringComparison.Ordinal));

    // Bounded scalar CFG trace for these fixtures; unknown instructions fail the test.
    private static int Trace(GuestModule module, int flags, List<int> calls)
    {
        GuestFunction function = TestFunction(module);
        Dictionary<string, int> values = function.Locals.ToDictionary(item => item.Id, _ => 0);
        Dictionary<string, int> globals = module.Globals.ToDictionary(item => item.Id, _ => 0);
        Dictionary<string, GuestBasicBlock> blocks = function.Blocks.ToDictionary(item => item.Id);
        GuestBasicBlock block = blocks[function.EntryBlockId];
        for (int step = 0; step < 100; ++step)
        {
            foreach (GuestInstruction instruction in block.Instructions)
            {
                int[] operands = instruction.OperandIds.Select(id => values[id]).ToArray();
                switch (instruction.Op)
                {
                    case "constant":
                        values[instruction.ResultId!] = int.Parse(instruction.Constant!.Value!, CultureInfo.InvariantCulture);
                        break;
                    case "local_load": values[instruction.ResultId!] = values[instruction.TargetId!]; break;
                    case "local_store": values[instruction.TargetId!] = operands[0]; break;
                    case "global_load": values[instruction.ResultId!] = globals[instruction.TargetId!]; break;
                    case "global_store": globals[instruction.TargetId!] = operands[0]; break;
                    case "binary":
                        Assert(instruction.OperatorKind == "not_equals", "unexpected fixture binary operator");
                        values[instruction.ResultId!] = operands[0] != operands[1] ? 1 : 0;
                        break;
                    case "call":
                        if (instruction.TargetId!.Contains(".ReadFlag(", StringComparison.Ordinal))
                        {
                            calls.Add(operands[0]);
                            values[instruction.ResultId!] = (flags >> operands[0]) & 1;
                        }
                        else
                        {
                            Assert(instruction.TargetId.Contains(".First(", StringComparison.Ordinal), "unexpected fixture call");
                            values[instruction.ResultId!] = operands[0];
                        }
                        break;
                    default: throw new InvalidOperationException($"Unexpected fixture opcode: {instruction.Op}");
                }
            }
            GuestTerminator terminator = block.Terminator;
            if (terminator.Kind == "return") return values[terminator.ReturnValueId!];
            Assert(terminator.Kind is "branch" or "branch_if", "unexpected fixture terminator");
            string next = terminator.Kind == "branch_if" && values[terminator.ConditionValueId!] == 0
                ? terminator.FalseTargetBlockId! : terminator.TargetBlockId!;
            block = blocks[next];
        }
        throw new InvalidOperationException("Fixture CFG trace exceeded its step budget.");
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}
