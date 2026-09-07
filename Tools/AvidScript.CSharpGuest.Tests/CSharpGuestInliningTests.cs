using System;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

internal static class CSharpGuestInliningTests
{
    private static readonly string SemanticHash = new('c', 64);

    public static int Run()
    {
        SmallArithmeticLeafIsInlined();
        BranchingLeafIsRejected();
        DebugInstrumentationRetainsCallBoundary();
        return 3;
    }

    private static void SmallArithmeticLeafIsInlined()
    {
        SemanticDocument semantic = Analyze(ArithmeticSource, "Scripts/LeafInline.cs");
        GuestModule baseline = CSharpGuestLowerer.Lower(
            semantic,
            SemanticHash,
            enableLeafFunctionInlining: false).Module
            ?? throw new InvalidOperationException("leaf inline baseline produced no Guest module");
        GuestModule optimized = CSharpGuestLowerer.Lower(semantic, SemanticHash).Module
            ?? throw new InvalidOperationException("leaf inline candidate produced no Guest module");

        GuestFunction baselineEntry = FindExportFunction(baseline, "leaf_inline");
        GuestFunction optimizedEntry = FindExportFunction(optimized, "leaf_inline");
        Assert(CallsMethod(baselineEntry, ".Mix("),
            "disabled leaf inlining should retain the arithmetic helper call");
        Assert(!CallsMethod(optimizedEntry, ".Mix("),
            "enabled leaf inlining should remove the arithmetic helper call");
        Assert(optimizedEntry.Blocks
                .SelectMany(block => block.Instructions)
                .Count(instruction => instruction.Op == "binary") >= 2,
            "inlined arithmetic should preserve the multiply and add operations");
        Assert(GuestModuleValidator.Validate(optimized).Succeeded,
            "inlined Guest IR should pass independent validation");
    }

    private static void BranchingLeafIsRejected()
    {
        SemanticDocument semantic = Analyze(BranchingSource, "Scripts/BranchingLeaf.cs");
        GuestModule module = CSharpGuestLowerer.Lower(semantic, SemanticHash).Module
            ?? throw new InvalidOperationException("branching leaf source produced no Guest module");

        Assert(CallsMethod(FindExportFunction(module, "branching_leaf"), ".Choose("),
            "branching helpers must retain their call boundary");
    }

    private static void DebugInstrumentationRetainsCallBoundary()
    {
        SemanticDocument semantic = Analyze(ArithmeticSource, "Scripts/DebugLeafInline.cs");
        GuestModule module = CSharpGuestLowerer.Lower(
            semantic,
            SemanticHash,
            enableDebugInstrumentation: true).Module
            ?? throw new InvalidOperationException("debug leaf source produced no Guest module");

        Assert(CallsMethod(FindExportFunction(module, "leaf_inline"), ".Mix("),
            "debug instrumentation should retain source-level helper call boundaries");
    }

    private static GuestFunction FindExportFunction(GuestModule module, string exportName)
    {
        string functionId = module.Exports.Single(exportItem => exportItem.Name == exportName).FunctionId;
        return module.Functions.Single(function => function.Id == functionId);
    }

    private static bool CallsMethod(GuestFunction function, string methodName)
    {
        return function.Blocks
            .SelectMany(block => block.Instructions)
            .Any(instruction => instruction.Op == "call"
                && instruction.TargetId?.Contains(methodName, StringComparison.Ordinal) == true);
    }

    private static SemanticDocument Analyze(string source, string sourceId)
    {
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        SemanticDocument semantic = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);
        Assert(semantic.Succeeded, $"{sourceId} should produce a valid semantic artifact");
        return semantic;
    }

    private const string ArithmeticSource = """
        using System.Runtime.InteropServices;

        namespace Game;

        public static class Script
        {
            [UnmanagedCallersOnly(EntryPoint = "leaf_inline")]
            public static int Run(int value)
            {
                return Mix(value);
            }

            private static int Mix(int value)
            {
                return value * 1664525 + 1013904223;
            }
        }
        """;

    private const string BranchingSource = """
        using System.Runtime.InteropServices;

        namespace Game;

        public static class Script
        {
            [UnmanagedCallersOnly(EntryPoint = "branching_leaf")]
            public static int Run(int value)
            {
                return Choose(value);
            }

            private static int Choose(int value)
            {
                if (value >= 0)
                {
                    return value;
                }
                return 0 - value;
            }
        }
        """;

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
