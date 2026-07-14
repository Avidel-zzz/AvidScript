using System;
using System.Linq;
using AvidScript.GuestIr;

internal static class GuestValidatorRobustnessTests
{
    public static int Run()
    {
        DuplicateReferencedGlobalReturnsDiagnostics();
        return 1;
    }

    private static void DuplicateReferencedGlobalReturnsDiagnostics()
    {
        GuestModule module = GuestModuleValidationTests.CreateMinimalModule();
        GuestGlobal global = new(
            "global:score",
            "type:int32",
            true,
            new GuestConstant("int32", "0"));
        GuestFunction function = module.Functions[0];
        GuestBasicBlock block = function.Blocks[0] with
        {
            Instructions = new[]
            {
                new GuestInstruction(
                    "global_load",
                    "value:result",
                    Array.Empty<string>(),
                    global.Id,
                    null,
                    null),
            },
        };
        function = function with { Blocks = new[] { block } };
        module = module with
        {
            Globals = new[] { global, global },
            Functions = new[] { function },
        };

        GuestValidationResult result = GuestModuleValidator.Validate(module);

        Assert(!result.Succeeded && result.Diagnostics.Any(item => item.Code == "ASIR1002"),
            "duplicate referenced globals should return diagnostics instead of throwing");
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
