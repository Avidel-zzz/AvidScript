using System;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

internal static class CSharpGuestOperatorTests
{
    private const string PointTypeId = "type:global::Game.Point";
    private const string OperatorId = "symbol:method:global::Game.Point.op_Addition(Point,Point):Point";
    private const string CallerId = "symbol:method:global::Game.Script.Add(Point,Point):Point";
    private const string LeftId = "symbol:parameter:caller:0";
    private const string RightId = "symbol:parameter:caller:1";
    private static readonly string SemanticHash = new('c', 64);

    public static int Run()
    {
        BoundUserOperatorLowersToCallable();
        IntegralShiftCountWidensToLeftOperandType();
        BuiltInUnaryOperatorsLowerToGuestPrimitives();
        IncrementAndDecrementPreserveValueSemantics();
        DirectBaseHandleUpcastsLowerToGuestCalls();
        MissingUserDefinedConversionFailsClosed();
        MalformedUserDefinedConversionsFailClosed();
        return 7;
    }

    private static void BoundUserOperatorLowersToCallable()
    {
        SemanticDocument baseline = CSharpGuestSemanticFixture.Create();
        SemanticCallable operatorCallable = new(
            OperatorId,
            PointTypeId,
            PointTypeId,
            new[]
            {
                new SemanticCallableParameter(0, "symbol:parameter:operator:0", "left", PointTypeId, "none"),
                new SemanticCallableParameter(1, "symbol:parameter:operator:1", "right", PointTypeId, "none"),
            },
            true,
            false,
            false,
            null,
            new SemanticCallableImport("env", "point_add"),
            null);
        SemanticCallable caller = new(
            CallerId,
            "type:global::Game.Script",
            PointTypeId,
            new[]
            {
                new SemanticCallableParameter(0, LeftId, "left", PointTypeId, "none"),
                new SemanticCallableParameter(1, RightId, "right", PointTypeId, "none"),
            },
            true,
            false,
            true,
            null,
            null,
            null);
        SemanticOperation addition = CSharpGuestSemanticFixture.Operation(
            "binary",
            PointTypeId,
            OperatorId,
            new[]
            {
                CSharpGuestSemanticFixture.Operation("parameter_reference", PointTypeId, LeftId),
                CSharpGuestSemanticFixture.Operation("parameter_reference", PointTypeId, RightId),
            },
            operatorKind: "add");
        SemanticDocument document = baseline with
        {
            Callables = baseline.Callables.Concat(new[] { operatorCallable, caller }).ToArray(),
            ControlFlowGraphs = baseline.ControlFlowGraphs.Append(ReturnGraph(CallerId, addition)).ToArray(),
        };

        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(document, SemanticHash);
        GuestModule module = result.Module
            ?? throw new InvalidOperationException(string.Join(
                " | ",
                result.Diagnostics.Select(item => $"{item.Code}:{item.Message}")));
        GuestFunction function = module.Functions.Single(item => item.Id == $"function:{CallerId}");
        GuestInstruction[] instructions = function.Blocks.SelectMany(block => block.Instructions).ToArray();

        Assert(result.Succeeded && GuestModuleValidator.Validate(module).Succeeded,
            "bound operator module should lower and validate");
        Assert(instructions.Any(item => item.Op == "call" && item.TargetId == $"import:{OperatorId}")
            && instructions.All(item => item.Op != "binary"),
            "user-defined operator should lower to its Roslyn-bound callable");
    }

    private static void IntegralShiftCountWidensToLeftOperandType()
    {
        const string source = """
            using System.Runtime.InteropServices;

            namespace AvidScript;

            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "avid_unpack_generation")]
                public static long UnpackGeneration(long packedHandle) => packedHandle >> 32;
            }
            """;
        const string sourceId = "Scripts/IntegralShift.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        SemanticDocument semantic = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);
        Assert(semantic.Succeeded,
            "integral shift source should pass semantic analysis: "
                + string.Join(" | ", semantic.Diagnostics.Select(item => item.Code + ":" + item.Message)));

        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(semantic, SemanticHash);
        GuestModule module = result.Module
            ?? throw new InvalidOperationException(string.Join(
                " | ",
                result.Diagnostics.Select(item => $"{item.Code}:{item.Message}")));
        GuestFunction function = module.Exports
            .Where(export => export.Name == "avid_unpack_generation")
            .Select(export => module.Functions.Single(item => item.Id == export.FunctionId))
            .Single();
        GuestInstruction[] instructions = function.Blocks.SelectMany(block => block.Instructions).ToArray();
        GuestInstruction shift = instructions.Single(item =>
            item.Op == "binary" && item.OperatorKind == "right_shift");
        GuestInstruction conversion = instructions.Single(item =>
            item.Op == "convert" && item.ResultId == shift.OperandIds[1]);

        Assert(result.Succeeded && GuestModuleValidator.Validate(module).Succeeded,
            "integral shift count should lower to valid Guest IR");
        Assert(conversion.OperandIds.Count == 1,
            "integral shift count should be explicitly widened before the binary instruction");
    }

    private static void BuiltInUnaryOperatorsLowerToGuestPrimitives()
    {
        const string source = """
            using System.Runtime.InteropServices;

            namespace AvidScript;

            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "avid_unary_int")]
                public static int UnaryInt(int value)
                {
                    int negated = -value;
                    int complemented = ~value;
                    return +negated + complemented;
                }

                [UnmanagedCallersOnly(EntryPoint = "avid_unary_bool")]
                public static bool UnaryBool(bool value) => !value;
            }
            """;
        const string sourceId = "Scripts/BuiltInUnary.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        SemanticDocument semantic = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);
        Assert(semantic.Succeeded,
            "built-in unary source should pass semantic analysis: "
                + string.Join(" | ", semantic.Diagnostics.Select(item => item.Code + ":" + item.Message)));

        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(semantic, SemanticHash);
        GuestModule module = result.Module
            ?? throw new InvalidOperationException(string.Join(
                " | ",
                result.Diagnostics.Select(item => $"{item.Code}:{item.Message}")));
        GuestInstruction[] instructions = module.Functions
            .SelectMany(function => function.Blocks)
            .SelectMany(block => block.Instructions)
            .ToArray();

        Assert(result.Succeeded && GuestModuleValidator.Validate(module).Succeeded,
            "built-in unary operators should lower to valid Guest IR");
        Assert(instructions.Any(item => item.Op == "binary" && item.OperatorKind == "subtract")
            && instructions.Any(item => item.Op == "binary" && item.OperatorKind == "bitwise_xor")
            && instructions.Any(item => item.Op == "binary" && item.OperatorKind == "equals"),
            "negate, bitwise-not, and logical-not should lower to existing Guest binary primitives");
    }

    private static void IncrementAndDecrementPreserveValueSemantics()
    {
        const string source = """
            using System.Runtime.InteropServices;

            namespace AvidScript;

            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "avid_step")]
                public static int Step(int value)
                {
                    int postfixIncrement = value++;
                    int prefixIncrement = ++value;
                    int postfixDecrement = value--;
                    int prefixDecrement = --value;
                    return postfixIncrement
                        + prefixIncrement
                        + postfixDecrement
                        + prefixDecrement
                        + value;
                }
            }
            """;
        const string sourceId = "Scripts/IncrementAndDecrement.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        SemanticDocument semantic = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);
        Assert(semantic.Succeeded,
            "increment/decrement source should pass semantic analysis: "
                + string.Join(" | ", semantic.Diagnostics.Select(item => item.Code + ":" + item.Message)));

        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(semantic, SemanticHash);
        GuestModule module = result.Module
            ?? throw new InvalidOperationException(string.Join(
                " | ",
                result.Diagnostics.Select(item => $"{item.Code}:{item.Message}")));
        GuestInstruction[] instructions = module.Functions
            .SelectMany(function => function.Blocks)
            .SelectMany(block => block.Instructions)
            .ToArray();

        Assert(result.Succeeded && GuestModuleValidator.Validate(module).Succeeded,
            "increment/decrement operators should lower to valid Guest IR");
        Assert(instructions.Count(item => item.Op == "binary" && item.OperatorKind == "add") >= 2
            && instructions.Count(item => item.Op == "binary" && item.OperatorKind == "subtract") >= 2,
            "increment/decrement should lower to existing add/subtract primitives");
    }

    private static void DirectBaseHandleUpcastsLowerToGuestCalls()
    {
        const string source = """
            using System.Runtime.InteropServices;

            namespace AvidScript;

            public readonly struct UObject
            {
                private readonly int Slot;
                private readonly int Generation;
                internal UObject(int slot, int generation) { Slot = slot; Generation = generation; }
                internal int AvidScriptSlot => Slot;
            }

            public readonly struct AActor
            {
                private readonly int Slot;
                private readonly int Generation;
                internal AActor(int slot, int generation) { Slot = slot; Generation = generation; }
                internal int AvidScriptSlot => Slot;
                public static implicit operator UObject(AActor value) => new(value.Slot, value.Generation);
            }

            public readonly struct AStaticMeshActor
            {
                private readonly int Slot;
                private readonly int Generation;
                internal AStaticMeshActor(int slot, int generation) { Slot = slot; Generation = generation; }
                public static implicit operator AActor(AStaticMeshActor value) => new(value.Slot, value.Generation);
            }

            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "avid_direct_base")]
                public static int DirectBase(AStaticMeshActor value)
                {
                    AActor actor = value;
                    return actor.AvidScriptSlot;
                }

                [UnmanagedCallersOnly(EntryPoint = "avid_explicit_chain")]
                public static int ExplicitChain(AStaticMeshActor value)
                {
                    AActor actor = value;
                    UObject root = actor;
                    return root.AvidScriptSlot;
                }
            }
            """;
        const string sourceId = "Scripts/HandleUpcasts.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        SemanticDocument semantic = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);
        Assert(semantic.Succeeded,
            "direct-base handle upcast source should pass semantic analysis: "
                + string.Join(" | ", semantic.Diagnostics.Select(item => item.Code + ":" + item.Message)));

        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(semantic, SemanticHash);
        GuestModule module = result.Module
            ?? throw new InvalidOperationException(string.Join(
                " | ",
                result.Diagnostics.Select(item => $"{item.Code}:{item.Message}")));
        GuestInstruction[] instructions = module.Functions
            .SelectMany(function => function.Blocks)
            .SelectMany(block => block.Instructions)
            .ToArray();
        string[] conversionIds = semantic.Callables
            .Where(callable => callable.MethodSymbolId.Contains("op_Implicit", StringComparison.Ordinal))
            .Select(callable => callable.MethodSymbolId)
            .ToArray();

        Assert(result.Succeeded && GuestModuleValidator.Validate(module).Succeeded,
            "direct-base handle upcasts should lower to valid Guest IR");
        Assert(conversionIds.Length == 2
            && conversionIds.All(id => module.Functions.Any(function => function.Id == $"function:{id}"))
            && conversionIds.All(id => instructions.Any(instruction => instruction.Op == "call"
                && instruction.TargetId == $"function:{id}")),
            "each explicit direct-base conversion should be a regular Guest function call");
        Assert(module.Imports.Count == 0
            && instructions.All(instruction => instruction.TargetId is null || !instruction.TargetId.StartsWith("import:", StringComparison.Ordinal)),
            "handle upcasts should not introduce Host imports");
        GuestInstruction[] callerInstructions = module.Exports
            .Where(export => export.Name is "avid_direct_base" or "avid_explicit_chain")
            .Select(export => module.Functions.Single(function => function.Id == export.FunctionId))
            .SelectMany(function => function.Blocks)
            .SelectMany(block => block.Instructions)
            .ToArray();
        Assert(callerInstructions.All(instruction => instruction.Op != "convert"),
            "direct-base handle upcast callers should not emit ordinary convert instructions");
    }

    private static void MissingUserDefinedConversionFailsClosed()
    {
        SemanticDocument baseline = AnalyzeConversionFixture();
        SemanticCallable conversion = GetConversionCallable(baseline);
        SemanticCallableParameter parameter = conversion.Parameters.Single();
        SemanticOperation conversionOperation = CSharpGuestSemanticFixture.Operation(
            "conversion",
            conversion.ReturnTypeId,
            children: new[] { CSharpGuestSemanticFixture.Operation(
                "parameter_reference", parameter.TypeId, parameter.SymbolId) },
            conversion: new SemanticConversion(
                "user_defined", true, false, true, false, false, false, true, conversion.MethodSymbolId + ":missing"));
        SemanticDocument document = baseline with
        {
            ControlFlowGraphs = baseline.ControlFlowGraphs
                .Where(graph => graph.MethodSymbolId != conversion.MethodSymbolId)
                .Append(ReturnGraph(conversion.MethodSymbolId, conversionOperation))
                .ToArray(),
        };

        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(document, SemanticHash);

        Assert(!result.Succeeded && result.Diagnostics.Any(item => item.Code == "ASCG1004"),
            "a missing user-defined conversion target must fail with ASCG1004");
    }

    private static void MalformedUserDefinedConversionsFailClosed()
    {
        SemanticDocument baseline = AnalyzeConversionFixture();
        SemanticCallable conversion = GetConversionCallable(baseline);
        SemanticCallableParameter parameter = conversion.Parameters.Single();

        SemanticDocument unreachable = baseline with
        {
            Reachability = baseline.Reachability! with
            {
                ReachableCallableIds = baseline.Reachability!.ReachableCallableIds
                    .Where(id => id != conversion.MethodSymbolId)
                    .ToArray(),
            },
        };
        AssertConversionRejected(unreachable,
            "an unreachable user-defined conversion callable");

        SemanticCallable importedConversion = conversion with
        {
            HasBody = false,
            Import = new SemanticCallableImport("env", "host_convert"),
        };
        SemanticDocument imported = ReplaceCallable(baseline, importedConversion);
        imported = imported with
        {
            Reachability = imported.Reachability! with
            {
                ReachableImports = new[]
                {
                    new SemanticReachableImport(
                        importedConversion.MethodSymbolId,
                        importedConversion.Import!.Module,
                        importedConversion.Import.Name),
                },
            },
        };
        AssertConversionRejected(imported,
            "an imported user-defined conversion callable");

        AssertConversionRejected(
            ReplaceCallable(baseline, conversion with
            {
                Parameters = new[] { parameter with { RefKind = "ref" } },
            }),
            "a ref user-defined conversion callable");

        AssertConversionRejected(
            ReplaceCallable(baseline, conversion with { IsStatic = false }),
            "an instance user-defined conversion callable");

        AssertConversionRejected(
            ReplaceCallable(baseline, conversion with { HasBody = false }),
            "a user-defined conversion callable without a Guest body");
    }

    private static SemanticDocument AnalyzeConversionFixture()
    {
        const string source = """
            using System.Runtime.InteropServices;

            namespace AvidScript;

            public readonly struct Target
            {
                private readonly int Slot;
                private readonly int Generation;
                internal Target(int slot, int generation) { Slot = slot; Generation = generation; }
            }

            public readonly struct Source
            {
                private readonly int Slot;
                private readonly int Generation;
                internal Source(int slot, int generation) { Slot = slot; Generation = generation; }
                public static implicit operator Target(Source value) => new(value.Slot, value.Generation);
            }

            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "avid_missing_conversion")]
                public static Target Convert(Source value) => value;
            }
            """;
        const string sourceId = "Scripts/MissingConversion.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        SemanticDocument semantic = SemanticAnalyzer.Analyze(
            source,
            sourceId,
            frontend.Source.Sha256);
        Assert(semantic.Succeeded,
            "conversion fixture should pass semantic analysis: "
                + string.Join(" | ", semantic.Diagnostics.Select(item => item.Code + ":" + item.Message)));
        return semantic;
    }

    private static SemanticCallable GetConversionCallable(SemanticDocument document)
    {
        return document.Callables.Single(callable =>
            callable.MethodSymbolId.Contains("Source.op_Implicit", StringComparison.Ordinal));
    }

    private static SemanticDocument ReplaceCallable(
        SemanticDocument document,
        SemanticCallable replacement)
    {
        return document with
        {
            Callables = document.Callables
                .Select(callable => callable.MethodSymbolId == replacement.MethodSymbolId
                    ? replacement
                    : callable)
                .ToArray(),
        };
    }

    private static void AssertConversionRejected(SemanticDocument document, string description)
    {
        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(document, SemanticHash);
        Assert(!result.Succeeded
            && result.Module is null
            && result.Diagnostics.Any(item => item.Code == "ASCG1004")
            && result.Diagnostics.All(item => item.Code != "ASCG1006"),
            description + " must fail with ASCG1004 before emitting or validating a call: "
                + string.Join(" | ", result.Diagnostics.Select(item => item.Code + ":" + item.Message)));
    }

    private static SemanticControlFlowGraph ReturnGraph(string methodSymbolId, SemanticOperation returnValue)
    {
        SemanticControlFlowEdge entry = new(0, 1, "fallthrough", "regular");
        SemanticControlFlowEdge returned = new(1, 2, "fallthrough", "return");
        return new SemanticControlFlowGraph(
            methodSymbolId,
            0,
            2,
            new[]
            {
                new SemanticBasicBlock(0, "entry", true, "none", Array.Empty<SemanticOperation>(), null,
                    Array.Empty<SemanticControlFlowEdge>(), new[] { entry }),
                new SemanticBasicBlock(1, "block", true, "none", Array.Empty<SemanticOperation>(), returnValue,
                    new[] { entry }, new[] { returned }),
                new SemanticBasicBlock(2, "exit", true, "none", Array.Empty<SemanticOperation>(), null,
                    new[] { returned }, Array.Empty<SemanticControlFlowEdge>()),
            });
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
