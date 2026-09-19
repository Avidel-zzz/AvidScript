using System;
using System.Linq;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;

internal static class CSharpGuestClosureContractTests
{
    public static int Run()
    {
        SemanticDocument document = CSharpGuestLexicalCaptureTests.Analyze("""
            using System;
            using System.Runtime.InteropServices;
            public static class Script {
                [UnmanagedCallersOnly(EntryPoint = "run")]
                public static int Run(int value) {
                    int Read() => ++value;
                    Func<int> callback = Read;
                    return callback();
                }
            }
            """);
        CSharpGuestLoweringResult pending = CSharpGuestLowerer.Lower(document, new string('a', 64));
        Require(!pending.Succeeded && pending.Module is null && pending.Diagnostics.Any(item => item.Code == "ASCG1024"),
            "a valid closure plan must not silently fall back to stack addresses");
        SemanticClosureEnvironment environment = document.ClosureEnvironments.Single();
        foreach (SemanticDocument malformed in new[]
        {
            document with { ClosureEnvironments = Array.Empty<SemanticClosureEnvironment>(), ClosureBindings = Array.Empty<SemanticClosureBinding>() },
            document with { ClosureEnvironments = new[] { environment with { Id = "forged" } } },
            document with { ClosureBindings = Array.Empty<SemanticClosureBinding>() },
            document with { ClosureBindings = null! },
            document with { SchemaVersion = 21, SemanticVersion = "1.25" },
        })
        {
            CSharpGuestLoweringResult rejected = CSharpGuestLowerer.Lower(malformed, new string('a', 64));
            Require(!rejected.Succeeded && rejected.Module is null && rejected.Diagnostics.Any(item => item.Code == "ASCG1001"),
                "stripped, forged or downgraded environments must fail before lowering");
        }
        return 6;
    }

    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}
