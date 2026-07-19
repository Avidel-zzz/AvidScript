using System;
using System.Linq;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

internal static class CSharpGuestStateSchemaTests
{
    public static int Run()
    {
        SchemaIncludesOnlySafeStateOwnedByTheScriptType();
        TypeFingerprintChangesWithValueLayout();
        return 2;
    }

    private static void SchemaIncludesOnlySafeStateOwnedByTheScriptType()
    {
        SemanticDocument baseline = CSharpGuestSemanticFixture.Create();
        SemanticSymbol unsafeArray = new(
            "symbol:field:global::Game.Script.Values:int32[]",
            "field",
            "Values",
            "symbol:type:global::Game.Script",
            "type:int32[]",
            "int32[] Values",
            true,
            "private",
            baseline.Symbols[0].Span);
        SemanticSymbol foreignState = new(
            "symbol:field:global::Game.Other.Score:int32",
            "field",
            "Score",
            "symbol:type:global::Game.Other",
            "type:int32",
            "int32 Score",
            true,
            "private",
            baseline.Symbols[0].Span);
        SemanticDocument document = baseline with
        {
            Symbols = baseline.Symbols.Concat(new[] { unsafeArray, foreignState }).ToArray(),
        };
        GuestModule module = CSharpGuestLowerer.Lower(document, new string('c', 64)).Module
            ?? throw new InvalidOperationException("Fixture should lower.");

        CSharpGuestStateSchema first = CSharpGuestStateSchemaProjector.Project(document, module);
        CSharpGuestStateSchema second = CSharpGuestStateSchemaProjector.Project(document, module);

        Assert(first.SchemaVersion == 1 && first.Strategy == "host_snapshot",
            "schema should opt into the versioned host snapshot strategy");
        Assert(first.OwnerTypeId == "type:global::Game.Script",
            "schema owner should come from the exported script lifecycle type");
        Assert(first.Slots.Count == 1
            && first.Slots[0].StableId == "state:type:global::Game.Script:Score"
            && !first.Slots[0].StableId.Contains("int32", StringComparison.Ordinal),
            "stable state identity should use owner and field name without embedding its type");
        Assert(first.Slots[0].TypeFingerprint.Length == 64
            && first.Slots[0].TypeFingerprint.All(Uri.IsHexDigit),
            "state type fingerprint should be lowercase SHA-256");
        Assert(CSharpGuestStateSchemaSerializer.Serialize(first)
                .SequenceEqual(CSharpGuestStateSchemaSerializer.Serialize(second)),
            "equivalent semantic and Guest IR input should produce byte-identical schema");
    }

    private static void TypeFingerprintChangesWithValueLayout()
    {
        SemanticDocument document = CSharpGuestSemanticFixture.Create();
        GuestModule module = CSharpGuestLowerer.Lower(document, new string('d', 64)).Module
            ?? throw new InvalidOperationException("Fixture should lower.");
        CSharpGuestStateSchema baseline = CSharpGuestStateSchemaProjector.Project(document, module);
        GuestModule changedTypeModule = module with
        {
            Types = module.Types.Select(type => type.Id == "type:int32"
                ? type with { Storage = "i64", Size = 8, Alignment = 8 }
                : type).ToArray(),
        };
        CSharpGuestStateSchema changed = CSharpGuestStateSchemaProjector.Project(document, changedTypeModule);

        Assert(baseline.Slots[0].TypeFingerprint != changed.Slots[0].TypeFingerprint,
            "storage and layout changes should invalidate a stable field type fingerprint");
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
