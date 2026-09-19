using System;
using System.Linq;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.WasmBackend;

internal static class CSharpGuestDelegateSignatureTests
{
    public static int Run()
    {
        SemanticDocument baseline = CSharpGuestLexicalCaptureTests.Analyze(
            "using System.Runtime.InteropServices; public static class Script { [UnmanagedCallersOnly(EntryPoint = \"run\")] public static int Run() => 5; }");
        Require(CSharpGuestLowerer.Lower(baseline, new string('d', 64)).Succeeded, "current baseline must lower");
        SemanticType delegateType = new("type:TestDelegate", "TestDelegate", "TestDelegate", "delegate", false, false);
        SemanticDelegateParameter[] parameters = { new(0, "type:int32", "ref") };
        SemanticDelegateType signature = new(delegateType.Id,
            SemanticDelegateType.GetInvokeId(delegateType.Id, "type:int32", "none", parameters),
            "type:int32", "none", parameters);
        SemanticDocument validMetadata = baseline with
        {
            Types = baseline.Types.Append(delegateType).ToArray(),
            DelegateTypes = new[] { signature },
        };
        Require(SemanticDelegateContractValidator.IsValid(validMetadata), "valid signature metadata should validate independently of execution support");
        SemanticDocument[] invalid =
        {
            validMetadata with { DelegateTypes = Array.Empty<SemanticDelegateType>() },
            validMetadata with { DelegateTypes = new[] { signature, signature } },
            validMetadata with { DelegateTypes = new[] { signature with { InvokeMethodSymbolId = "forged" } } },
            validMetadata with { DelegateTypes = new[] { signature with { ReturnTypeId = "type:missing" } } },
            validMetadata with { DelegateTypes = new[] { signature with { Parameters = new[] { parameters[0] with { Ordinal = 1 } } } } },
            validMetadata with { DelegateTypes = new[] { signature with { Parameters = new[] { parameters[0] with { RefKind = "out" } } } } },
            validMetadata with { DelegateTypes = new[] { signature with { Parameters = null! } } },
            validMetadata with { DelegateTypes = null! },
            validMetadata with { SchemaVersion = 20, SemanticVersion = "1.22" },
        };
        foreach (SemanticDocument document in invalid)
        {
            CSharpGuestLoweringResult rejected = CSharpGuestLowerer.Lower(document, new string('d', 64));
            Require(!rejected.Succeeded && rejected.Module is null
                && rejected.Diagnostics.Any(item => item.Code == "ASCG1001"),
                "missing, forged, or downgraded delegate contracts must fail before lowering");
        }
        SemanticDocument asyncCurrent = CSharpGuestContinuationTests.Analyze("""
            using System.Runtime.InteropServices;
            using AvidScript;
            public static class Script
            {
                public static int Result;
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static async void BeginPlay()
                {
                    int value = 5;
                    if (value > 0) { await AvidContinuations.NextTickAsync(); Result = value; }
                }
            }
            """, "Scripts/PreviousDelegateContract.cs");
        SemanticDocument legacy = asyncCurrent with
            { SchemaVersion = 20, SemanticVersion = "1.22", DelegateTypes = Array.Empty<SemanticDelegateType>() };
        CSharpGuestLoweringResult previous = CSharpGuestLowerer.Lower(legacy, new string('e', 64));
        Require(previous.Succeeded && WasmModuleCompiler.Compile(previous.Module!).Succeeded,
            "schema 20/1.22 CFG async artifacts must remain consumable");
        return 11;
    }

    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}
