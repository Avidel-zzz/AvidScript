using System;
using System.Linq;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;

internal static class CSharpGuestMalformedTests
{
    private static readonly string SemanticHash = new('c', 64);

    public static int Run()
    {
        MalformedSemanticObjectFailsClosed();
        return 1;
    }

    private static void MalformedSemanticObjectFailsClosed()
    {
        SemanticDocument malformed = CSharpGuestSemanticFixture.Create() with { Types = null! };
        CSharpGuestLoweringResult nullResult = CSharpGuestLowerer.Lower(malformed, SemanticHash);
        SemanticDocument duplicate = CSharpGuestSemanticFixture.Create();
        duplicate = duplicate with { Types = duplicate.Types.Append(duplicate.Types[0]).ToArray() };
        CSharpGuestLoweringResult duplicateResult = CSharpGuestLowerer.Lower(duplicate, SemanticHash);

        Assert(!nullResult.Succeeded && nullResult.Module is null
            && nullResult.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCG1001"),
            "null semantic collections should return ASCG1001 instead of throwing");
        Assert(!duplicateResult.Succeeded && duplicateResult.Module is null
            && duplicateResult.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCG1001"),
            "duplicate semantic identities should return ASCG1001 instead of throwing");
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
