using System;
using System.Linq;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

internal static class CSharpGuestLoweringTests
{
    private static readonly string SemanticHash = new('c', 64);

    public static int Run()
    {
        FailedSemanticDocumentIsRejected();
        TypesStateAndCallableAbiAreProjected();
        LoweringIsByteDeterministic();
        return 3;
    }

    private static void FailedSemanticDocumentIsRejected()
    {
        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(
            CSharpGuestSemanticFixture.Create(succeeded: false),
            SemanticHash);

        Assert(!result.Succeeded && result.Module is null,
            "failed semantic input should not produce Guest IR");
        Assert(result.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCG1001"),
            "failed semantic input should report ASCG1001");
    }

    private static void TypesStateAndCallableAbiAreProjected()
    {
        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(
            CSharpGuestSemanticFixture.Create(),
            SemanticHash);
        GuestModule module = result.Module
            ?? throw new InvalidOperationException("valid semantic input produced no Guest module");

        Assert(result.Succeeded, "valid semantic input should lower");
        Assert(GuestModuleValidator.Validate(module).Succeeded,
            "lowered Guest IR should pass the independent validator");
        GuestType point = module.Types.Single(type => type.Id == "type:global::Game.Point");
        Assert(point.Size == 8 && point.Fields.Select(field => field.Offset).SequenceEqual(new[] { 0, 4 }),
            "struct fields should use shared canonical layout");
        Assert(module.Types.Single(type => type.Id == "type:global::Game.Mode").Size == 4,
            "enum should use its semantic underlying type");
        Assert(module.Types.Single(type => type.Id == "type:int32[]").Size == 4,
            "array values should lower to header addresses");
        Assert(module.Globals.Count == 1 && module.Globals[0].Id.Contains("Score", StringComparison.Ordinal),
            "static script state should become a Guest global");
        Assert(module.Imports.Count == 1 && module.Imports[0].Name == "host_add",
            "semantic imports should become Guest imports");
        Assert(module.Exports.Count == 1 && module.Exports[0].Name == "guest_main",
            "semantic exports should target lowered Guest functions");
        Assert(module.Provenance.SemanticSha256 == SemanticHash
            && module.Provenance.SourceSha256 == new string('a', 64)
            && module.Provenance.FrontendSha256 == new string('b', 64),
            "lowered module should preserve the complete provenance chain");
    }

    private static void LoweringIsByteDeterministic()
    {
        SemanticDocument document = CSharpGuestSemanticFixture.Create();

        GuestModule first = CSharpGuestLowerer.Lower(document, SemanticHash).Module!;
        GuestModule second = CSharpGuestLowerer.Lower(document, SemanticHash).Module!;

        Assert(GuestIrSerializer.Serialize(first).SequenceEqual(GuestIrSerializer.Serialize(second)),
            "the same semantic artifact should produce identical Guest IR bytes");
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
