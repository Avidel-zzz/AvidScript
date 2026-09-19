using System;
using System.Linq;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;

internal static class CSharpGuestClassContractTests
{
    public static int Run()
    {
        SemanticDocument document = CSharpGuestLexicalCaptureTests.Analyze("""
            using System;
            using System.Runtime.InteropServices;
            public static class Script {
                [UnmanagedCallersOnly(EntryPoint = "run")]
                public static int Run() { int value = 7; Func<int> read = () => value; return read(); }
            }
            """);
        int count = 0;
        Check(CSharpGuestLowerer.Lower(document, new string('a', 64)).Succeeded, "current class facts preserve existing closure lowering");
        SemanticClassType owner = document.ClassTypes.Single(item => item.IsSourceDeclared);
        foreach (SemanticDocument malformed in new[]
        {
            document with { ClassTypes = null! },
            document with { ClassTypes = Array.Empty<SemanticClassType>() },
            document with { ClassTypes = document.ClassTypes.Append(owner).ToArray() },
            document with { ClassTypes = document.ClassTypes.Select(item => item == owner ? item with { BaseTypeId = owner.TypeId } : item).ToArray() },
            document with { SchemaVersion = 23, SemanticVersion = "1.27" },
        })
        {
            CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(malformed, new string('a', 64));
            Check(!result.Succeeded && result.Module is null && result.Diagnostics.Any(item => item.Code == "ASCG1001"),
                "malformed class contracts must be rejected before planning managed delegates");
        }
        SemanticDocument legacy = document with { SchemaVersion = 23, SemanticVersion = "1.27", ClassTypes = Array.Empty<SemanticClassType>() };
        Check(CSharpGuestLowerer.Lower(legacy, new string('a', 64)).Succeeded, "schema 23 retains its existing executable closure allocation contract");
        SemanticDocument plainLambda = CSharpGuestLexicalCaptureTests.Analyze("""
            using System;
            using System.Runtime.InteropServices;
            public static class Script {
                [UnmanagedCallersOnly(EntryPoint = "run")]
                public static int Run() { Func<int> read = () => 7; return read(); }
            }
            """);
        Check(CSharpGuestLowerer.Lower(plainLambda with { SchemaVersion = 23, SemanticVersion = "1.27",
            ClassTypes = Array.Empty<SemanticClassType>() }, new string('a', 64)).Succeeded,
            "schema 23 no-capture lambda creation and invocation retain the original function-reference ABI");
        SemanticDocument referenceObject = CSharpGuestLexicalCaptureTests.Analyze("""
            using System.Runtime.InteropServices;
            public class Counter { public int Value; }
            public static class Script {
                [UnmanagedCallersOnly(EntryPoint = "run")]
                public static int Run() { Counter c = new Counter(); c.Value = 7; return c.Value; }
            }
            """);
        Check(referenceObject.Succeeded && SemanticClassContractValidator.IsValid(referenceObject), "reference object facts are valid semantic output");
        CSharpGuestLoweringResult referenceResult = CSharpGuestLowerer.Lower(referenceObject, new string('a', 64));
        Check(referenceResult.Succeeded && referenceResult.Module is not null,
            "ordinary default construction now executes through the versioned class contract: "
                + string.Join(" | ", referenceResult.Diagnostics.Select(item => item.Code + ": " + item.Message)));
        return count;

        void Check(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); count++; }
    }
}
