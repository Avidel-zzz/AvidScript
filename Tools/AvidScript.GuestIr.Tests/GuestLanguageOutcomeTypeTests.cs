using System;
using System.Linq;
using System.Text;
using AvidScript.GuestIr;

internal static class GuestLanguageOutcomeTypeTests
{
    private const string Int = "type:int32";
    private const string Payload = "type:language_error_payload";
    private const string Root = "type:language_error_root";
    private const string Outcome = "type:language_outcome";

    public static int Run()
    {
        ValidOutcomeLayoutRoundTrips();
        VoidOutcomeHasNoValueField();
        OlderVersionRejectsOutcomeMetadata();
        MismatchedVersionIsRejected();
        MissingVersionedListIsRejected();
        DuplicateDeclarationsAreRejected();
        WrongFieldOrderIsRejected();
        WrongRootKindIsRejected();
        WrongValueTypeIsRejected();
        NullDescriptorFailsClosed();
        return 10;
    }

    private static void ValidOutcomeLayoutRoundTrips()
    {
        GuestModule module = CreateModule();
        AssertValid(module);
        GuestType type = module.Types.Single(item => item.Id == Outcome);
        Check(type.Size == 32 && type.Alignment == 8
            && type.Fields.Select(field => field.Offset).SequenceEqual(new[] { 0, 4, 8, 16, 24 }),
            "language outcome offsets must come from canonical Guest layout");
        byte[] bytes = GuestIrSerializer.Serialize(module);
        Check(Encoding.UTF8.GetString(bytes).Contains("\"language_outcome_types\"", StringComparison.Ordinal),
            "IR 15 must serialize its outcome descriptor");
        Check(bytes.SequenceEqual(GuestIrSerializer.Serialize(GuestIrSerializer.Deserialize(bytes))),
            "language outcome descriptor must round-trip canonically");
    }

    private static void VoidOutcomeHasNoValueField()
    {
        GuestModule module = CreateModule(valueTypeId: null);
        AssertValid(module);
        GuestType type = module.Types.Single(item => item.Id == Outcome);
        Check(type.Size == 24 && type.Fields.Count == 4,
            "void outcome must contain error state but no value field");
    }

    private static void OlderVersionRejectsOutcomeMetadata()
    {
        GuestModule module = CreateModule() with { SchemaVersion = 14, IrVersion = "1.13" };
        AssertError(module, "ASIR1024");
        GuestModule old = GuestModuleValidationTests.CreateMinimalModule();
        Check(!Encoding.UTF8.GetString(GuestIrSerializer.Serialize(old))
            .Contains("language_outcome_types", StringComparison.Ordinal),
            "legacy artifacts must retain their original serialized shape");
    }

    private static void MismatchedVersionIsRejected() =>
        AssertError(CreateModule() with { IrVersion = "1.13" }, "ASIR1001");

    private static void MissingVersionedListIsRejected() =>
        AssertError(CreateModule() with { LanguageOutcomeTypes = null }, "ASIR1024");

    private static void DuplicateDeclarationsAreRejected()
    {
        GuestModule module = CreateModule();
        AssertError(module with
        {
            LanguageOutcomeTypes = module.LanguageOutcomeTypes!.Concat(module.LanguageOutcomeTypes!).ToArray(),
        }, "ASIR1024");
    }

    private static void WrongFieldOrderIsRejected()
    {
        GuestModule module = CreateModule();
        GuestType type = module.Types.Single(item => item.Id == Outcome);
        GuestField[] fields = type.Fields.ToArray();
        fields[0] = fields[0] with { Name = "value" };
        AssertError(module with { Types = ReplaceType(module, type with { Fields = fields }) }, "ASIR1024");
    }

    private static void WrongRootKindIsRejected()
    {
        GuestModule module = CreateModule();
        GuestType type = module.Types.Single(item => item.Id == Outcome);
        GuestField[] fields = type.Fields.ToArray();
        fields[3] = fields[3] with { TypeId = Int };
        AssertError(module with { Types = ReplaceType(module, type with { Fields = fields }) }, "ASIR1024");
    }

    private static void WrongValueTypeIsRejected()
    {
        GuestModule module = CreateModule();
        AssertError(module with
        {
            LanguageOutcomeTypes = new[] { new GuestLanguageOutcomeType(Outcome, Payload) },
        }, "ASIR1024");
    }

    private static void NullDescriptorFailsClosed()
    {
        GuestModule module = CreateModule() with
        {
            LanguageOutcomeTypes = new GuestLanguageOutcomeType[] { null! },
        };
        AssertError(module, "ASIR1001");
    }

    private static GuestModule CreateModule(string? valueTypeId = Int)
    {
        GuestModule baseline = GuestModuleValidationTests.CreateMinimalModule();
        GuestType payload = new(Payload, "struct", "memory",
            new[] { Field("code", Int) }, null, null, 0, 1);
        GuestType root = new(Root, "managed_ref", "i64",
            Array.Empty<GuestField>(), Payload, null, 8, 8);
        GuestField[] fields = valueTypeId is null
            ? new[] { Field("status", Int), Field("error_type", Int),
                Field("source", Int), Field("error_root", Root) }
            : new[] { Field("status", Int), Field("error_type", Int),
                Field("source", Int), Field("error_root", Root), Field("value", valueTypeId) };
        GuestType outcome = new(Outcome, "struct", "memory", fields, null, null, 0, 1);
        GuestTypeLayoutResult layout = GuestDataLayout.ComputeTypes(
            baseline.Types.Concat(new[] { payload, root, outcome }).ToArray());
        Check(layout.Succeeded, "language outcome fixture layout must be canonical");
        GuestImport heap = new("import:managed_heap", GuestManagedHeap.ImportModule,
            GuestManagedHeap.ImportName, new[] { Int, Int, Int, Int }, Int);
        return baseline with
        {
            SchemaVersion = 15,
            IrVersion = "1.14",
            Types = layout.Types,
            Imports = new[] { heap },
            LanguageOutcomeTypes = new[] { new GuestLanguageOutcomeType(Outcome, valueTypeId) },
        };
    }

    private static GuestField Field(string name, string typeId) =>
        new($"field:{name}", name, typeId, 0);

    private static GuestType[] ReplaceType(GuestModule module, GuestType replacement) =>
        module.Types.Select(item => item.Id == replacement.Id ? replacement : item).ToArray();

    private static void AssertValid(GuestModule module)
    {
        GuestValidationResult result = GuestModuleValidator.Validate(module);
        Check(result.Succeeded, string.Join(" | ", result.Diagnostics.Select(item => item.Message)));
    }

    private static void AssertError(GuestModule module, string code)
    {
        GuestValidationResult result = GuestModuleValidator.Validate(module);
        Check(!result.Succeeded && result.Diagnostics.Any(item => item.Code == code),
            $"expected {code}: " + string.Join(" | ", result.Diagnostics.Select(item => item.Message)));
    }

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}
