using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestShortCircuitTests
{
    public static int Run()
    {
        Action[] tests =
        {
            BooleanBranchesExecuteOnlyRequiredOperands,
            NestedContextsPreserveEvaluationOrder,
            StructuredAndResumableLoopsPreserveOrder,
            EagerOperatorsAndSkippedFailuresRemainDistinct,
            MalformedConditionalOperatorsFailClosed,
        };
        foreach (Action test in tests)
        {
            test();
            Console.WriteLine($"AsyncShortCircuit: {test.Method.Name} passed");
        }
        Console.WriteLine($"AsyncShortCircuit: {tests.Length}/{tests.Length} passed");
        return tests.Length;
    }

    private static void BooleanBranchesExecuteOnlyRequiredOperands()
    {
        (string Expression, int[] Trace, bool Result)[] cases =
        {
            ("Mark(1, false) && Mark(2, true)", new[] { 1 }, false),
            ("Mark(1, true) || Mark(2, false)", new[] { 1 }, true),
            ("Mark(1, true) && Mark(2, false)", new[] { 1, 2 }, false),
            ("Mark(1, false) || Mark(2, true)", new[] { 1, 2 }, true),
            ("Mark(1, true) && (Mark(2, false) || Mark(3, true))", new[] { 1, 2, 3 }, true),
            ("!(Mark(1, false) && Mark(2, true)) || Mark(3, true)", new[] { 1 }, true),
        };
        foreach ((string expression, int[] trace, bool result) in cases)
        {
            Execute($"await AvidContinuations.NextTickAsync(); Ready = {expression};", trace, result,
                Array.Empty<int>());
        }
    }

    private static void NestedContextsPreserveEvaluationOrder()
    {
        Execute("""
            Ready = Mark(1, false) || Mark(2, true);
            await AvidContinuations.NextTickAsync();
            bool local = Mark(3, true) && Mark(4, true);
            Ready = local || Mark(5, false);
            Consume(Argument(6), Mark(7, false) || Mark(8, true), Argument(9));
            Ready = ReturnChoice(Mark(10, true));
            """, new[] { 1, 2, 3, 4, 6, 7, 8, 9, 10, 90 }, true, new[] { 1, 2 });
        Execute("""
            await AvidContinuations.NextTickAsync();
            Ready = (Mark(1, false) || Mark(2, true)) && (Mark(3, true) || Mark(4, false));
            """, new[] { 1, 2, 3 }, true);
    }

    private static void StructuredAndResumableLoopsPreserveOrder()
    {
        Execute("""
            await AvidContinuations.NextTickAsync();
            int index = 0;
            while (Mark(1, index < 2) && Mark(2, true)) { index++; }
            do { index++; } while (Mark(3, false) && Mark(4, true));
            for (int item = 0; Mark(5, item < 1) && Mark(6, true); item++) { }
            if (Mark(7, false) || Mark(8, true)) { Ready = true; }
            """, new[] { 1, 2, 1, 2, 1, 3, 5, 6, 5, 7, 8 }, true);
        Execute("""
            while (Mark(1, Count < 2) && Mark(2, true))
            {
                Count++;
                await AvidContinuations.NextTickAsync();
                Ready = Mark(3, false) || Mark(4, true);
            }
            """, new[] { 1, 2, 3, 4, 1, 2, 3, 4, 1 }, true, new[] { 1, 2 });
    }

    private static void EagerOperatorsAndSkippedFailuresRemainDistinct()
    {
        Execute("""
            await AvidContinuations.NextTickAsync();
            Ready = Mark(1, false) & Mark(2, true);
            Ready = Mark(3, true) | Mark(4, false);
            """, new[] { 1, 2, 3, 4 }, true);
        Execute("await AvidContinuations.NextTickAsync(); Ready = Mark(1, false) && Fail();",
            new[] { 1 }, false);
        Execute("await AvidContinuations.NextTickAsync(); Ready = Mark(1, true) || Fail();",
            new[] { 1 }, true);
    }

    private static void MalformedConditionalOperatorsFailClosed()
    {
        SemanticDocument document = Analyze(
            "await AvidContinuations.NextTickAsync(); Ready = Mark(1, false) && Mark(2, true);");
        SemanticAsyncMethod method = document.AsyncMethods.Single();
        SemanticAsyncSegment segment = method.Segments[1];
        SemanticAsyncStatement statement = segment.Statements.Single();
        SemanticOperation assignment = statement.Operation.Children.Single();
        SemanticOperation expression = assignment.Children[1];
        SemanticOperation[] invalid =
        {
            expression with { IsLifted = true },
            expression with { TypeId = "type:int32" },
            expression with { Children = new[] { expression.Children[0] } },
        };
        foreach (SemanticOperation malformed in invalid)
        {
            SemanticDocument tampered = document with
            {
                AsyncMethods = new[]
                {
                    method with
                    {
                        Segments = new[]
                        {
                            method.Segments[0],
                            segment with
                            {
                                Statements = new[]
                                {
                                    statement with
                                    {
                                        Operation = statement.Operation with
                                        {
                                            Children = new[]
                                            {
                                                assignment with { Children = new[] { assignment.Children[0], malformed } },
                                            },
                                        },
                                    },
                                },
                            },
                        },
                    },
                },
            };
            CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(tampered, new string('c', 64));
            Assert(!result.Succeeded && result.Module is null && result.Diagnostics.Count > 0,
                "malformed or lifted conditional operators must not publish a module");
        }
    }

    private static SemanticDocument Analyze(string body)
    {
        string source = $$"""
            using AvidScript;
            using System.Runtime.InteropServices;
            namespace Game;
            public static class Script
            {
                private static bool Ready;
                private static int Count;
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static async void BeginPlay() { {{body}} }
                private static bool Mark(int id, bool value) { Trace(id); return value; }
                private static int Argument(int id) { Trace(id); return id; }
                private static void Consume(int first, bool value, int last) { Ready = value; }
                private static bool ReturnChoice(bool value) { return value && Mark(90, true); }
                private static bool Fail() { Trace(999); int zero = 0; return 1 / zero > 0; }
                [DllImport("env", EntryPoint = "test_trace")]
                private static extern void Trace(int id);
            }
            """;
        return CSharpGuestContinuationTests.Analyze(source, "Scripts/AsyncShortCircuit.cs");
    }

    private static void Execute(string body, int[] expected, bool ready, int[]? beforeResume = null)
    {
        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(Analyze(body), new string('c', 64));
        Assert(result.Succeeded && result.Module is not null,
            string.Join(" | ", result.Diagnostics.Select(item => item.Message)));
        GuestModule module = result.Module!;
        Assert(GuestModuleValidator.Validate(module).Succeeded, "short-circuit CFG must validate");
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(module);
        Assert(wasm.Succeeded && wasm.Bytes.Length > 8, "short-circuit CFG must compile to real WASM");
        ScalarExecution execution = new(module);
        execution.Invoke(module.Exports.Single(export => export.Name == "avid_on_begin_play").FunctionId);
        if (beforeResume is not null)
        {
            Assert(execution.Trace.SequenceEqual(beforeResume),
                $"before resume: [{string.Join(',', execution.Trace)}] != [{string.Join(',', beforeResume)}]");
        }
        execution.ResumeAll();
        Assert(execution.Trace.SequenceEqual(expected),
            $"side effects: [{string.Join(',', execution.Trace)}] != [{string.Join(',', expected)}]");
        string readyId = module.Globals.Single(global => global.Id.Contains(".Ready:", StringComparison.Ordinal)).Id;
        Assert(execution.Globals[readyId] == (ready ? 1 : 0), "short-circuit result must be stored into Ready");
    }

    // Execute the actual scalar Guest CFG and guest callees. Only the trace and timer
    // imports are supplied by this bounded fixture; unknown operations fail the test.
    private sealed class ScalarExecution
    {
        private readonly GuestModule module;
        private readonly Queue<int> pending = new();
        private int remainingSteps = 10000;

        public ScalarExecution(GuestModule module)
        {
            this.module = module;
            Globals = module.Globals.ToDictionary(global => global.Id, _ => 0.0);
        }

        public List<int> Trace { get; } = new();
        public Dictionary<string, double> Globals { get; }

        public void ResumeAll()
        {
            for (int count = 0; pending.Count > 0; ++count)
            {
                Assert(count < 8, "fixture exceeded its continuation budget");
                Invoke($"function:synthetic:async_resume:{pending.Dequeue()}", 1);
            }
        }

        public double Invoke(string functionId, params double[] arguments)
        {
            GuestFunction function = module.Functions.Single(item => item.Id == functionId);
            Assert(function.Parameters.Count == arguments.Length, "fixture call arity mismatch");
            Dictionary<string, double> values = function.Locals.ToDictionary(local => local.Id, _ => 0.0);
            for (int index = 0; index < arguments.Length; ++index)
            {
                values[function.Parameters[index].Id] = arguments[index];
            }
            Dictionary<string, GuestBasicBlock> blocks = function.Blocks.ToDictionary(block => block.Id);
            GuestBasicBlock block = blocks[function.EntryBlockId];
            while (--remainingSteps > 0)
            {
                foreach (GuestInstruction instruction in block.Instructions)
                {
                    double[] operands = instruction.OperandIds.Select(id => values[id]).ToArray();
                    switch (instruction.Op)
                    {
                        case "constant":
                            values[instruction.ResultId!] = instruction.Constant!.Kind is "zero" or "null"
                                ? 0 : double.Parse(instruction.Constant.Value!, CultureInfo.InvariantCulture);
                            break;
                        case "local_load": values[instruction.ResultId!] = values[instruction.TargetId!]; break;
                        case "local_store": values[instruction.TargetId!] = operands[0]; break;
                        case "global_load": values[instruction.ResultId!] = Globals[instruction.TargetId!]; break;
                        case "global_store": Globals[instruction.TargetId!] = operands[0]; break;
                        case "convert": values[instruction.ResultId!] = operands[0]; break;
                        case "binary":
                            values[instruction.ResultId!] = Binary(instruction.OperatorKind!, operands[0], operands[1]);
                            break;
                        case "call":
                            GuestImport? import = module.Imports.SingleOrDefault(item => item.Id == instruction.TargetId);
                            double returned;
                            if (import is null)
                            {
                                returned = Invoke(instruction.TargetId!, operands);
                            }
                            else if (import.Name == "test_trace")
                            {
                                Trace.Add((int)operands[0]);
                                returned = 0;
                            }
                            else if (import.Name == "continuation_delay")
                            {
                                pending.Enqueue((int)operands[1]);
                                returned = 1;
                            }
                            else
                            {
                                throw new InvalidOperationException($"Unexpected fixture import: {import.Name}");
                            }
                            if (instruction.ResultId is { } resultId) values[resultId] = returned;
                            break;
                        default: throw new InvalidOperationException($"Unexpected fixture opcode: {instruction.Op}");
                    }
                }
                GuestTerminator terminator = block.Terminator;
                if (terminator.Kind == "return")
                {
                    return terminator.ReturnValueId is { } resultId ? values[resultId] : 0;
                }
                Assert(terminator.Kind is "branch" or "branch_if", $"Unexpected fixture terminator: {terminator.Kind}");
                block = blocks[terminator.Kind == "branch_if" && values[terminator.ConditionValueId!] == 0
                    ? terminator.FalseTargetBlockId! : terminator.TargetBlockId!];
            }
            throw new InvalidOperationException("Fixture CFG exceeded its execution budget.");
        }

        private static double Binary(string op, double left, double right) => op switch
        {
            "add" => left + right,
            "subtract" => left - right,
            "multiply" => left * right,
            "divide" => right == 0 ? throw new DivideByZeroException() : Math.Truncate(left / right),
            "equals" => left == right ? 1 : 0,
            "not_equals" => left != right ? 1 : 0,
            "less_than" => left < right ? 1 : 0,
            "less_than_or_equal" => left <= right ? 1 : 0,
            "greater_than" => left > right ? 1 : 0,
            "greater_than_or_equal" => left >= right ? 1 : 0,
            "logical_and" => left != 0 && right != 0 ? 1 : 0,
            "logical_or" => left != 0 || right != 0 ? 1 : 0,
            "bitwise_and" => (long)left & (long)right,
            "bitwise_or" => (long)left | (long)right,
            _ => throw new InvalidOperationException($"Unexpected fixture binary operator: {op}"),
        };
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}
