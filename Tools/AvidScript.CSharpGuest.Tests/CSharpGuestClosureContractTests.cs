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
        CSharpGuestLoweringResult lowered = CSharpGuestLowerer.Lower(document, new string('a', 64));
        Require(lowered.Succeeded && lowered.Module!.Types.Any(type => type.Kind == "managed_ref")
            && AvidScript.WasmBackend.WasmModuleCompiler.Compile(lowered.Module).Succeeded,
            "a valid closure plan must compile with traced heap environments");
        SemanticClosureEnvironment environment = document.ClosureEnvironments.Single();
        foreach (SemanticDocument malformed in new[]
        {
            document with { ClosureEnvironments = Array.Empty<SemanticClosureEnvironment>(), ClosureBindings = Array.Empty<SemanticClosureBinding>() },
            document with { ClosureEnvironments = new[] { environment with { Id = "forged" } } },
            document with { ClosureBindings = Array.Empty<SemanticClosureBinding>() },
            document with { ClosureBindings = null! },
            document with { SchemaVersion = 21, SemanticVersion = "1.25", ClassTypes = Array.Empty<SemanticClassType>() },
            document with { ClosureEnvironments = new[] { environment with { Allocation = null } } },
            document with { ClosureEnvironments = new[] { environment with { Allocation = environment.Allocation! with { Entries = Array.Empty<SemanticClosureEntry>() } } } },
            document with { SchemaVersion = 22, SemanticVersion = "1.26", ClassTypes = Array.Empty<SemanticClassType>() },
        })
        {
            CSharpGuestLoweringResult rejected = CSharpGuestLowerer.Lower(malformed, new string('a', 64));
            Require(!rejected.Succeeded && rejected.Module is null && rejected.Diagnostics.Any(item => item.Code == "ASCG1001"),
                "stripped, forged or downgraded environments must fail before lowering");
        }
        SemanticDocument legacy = document with { SchemaVersion = 22, SemanticVersion = "1.26",
            ClassTypes = Array.Empty<SemanticClassType>(),
            ClosureEnvironments = new[] { environment with { Allocation = null } } };
        Require(CSharpGuestLowerer.Lower(legacy, new string('a', 64)).Diagnostics.Any(item => item.Code == "ASCG1024"),
            "legacy capture plans remain readable but cannot execute with missing allocation metadata");
        return 10;
    }

    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}
