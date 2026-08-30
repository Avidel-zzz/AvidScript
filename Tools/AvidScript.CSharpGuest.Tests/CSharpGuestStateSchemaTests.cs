using System;
using System.IO;
using System.Linq;
using System.Text;
using AvidScript.CSharpGuest;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

internal static class CSharpGuestStateSchemaTests
{
    public static int Run()
    {
        LegacyContractsProduceCompatibleSchemaV2();
        ExplicitContractsRequirePersistedFields();
        TransientConstAndReadonlyFieldsAreExcluded();
        AliasesExpandToStableIdsAndSerializeInFixedOrder();
        AliasAndCurrentStableIdsMustNotConflict();
        ExplicitUnsafePersistFailsWithAsState1005();
        CompatibleUnsafeImplicitFieldsAreSkipped();
        ContractVersionBoundsUseAsState1003();
        SourcePipelineProjectsExplicitStateSchemaV2();
        DelegateEventOnlyScriptDefinesStateOwner();
        CompatiblePersistedUnsafeSourceFailsWithAsState1005();
        TypeFingerprintChangesWithValueLayout();
        return 12;
    }

    private static void LegacyContractsProduceCompatibleSchemaV2()
    {
        SemanticDocument document = CSharpGuestSemanticFixture.Create();
        GuestModule module = Lower(document, 'c');

        CSharpGuestStateSchema first = CSharpGuestStateSchemaProjector.Project(document, module);
        CSharpGuestStateSchema second = CSharpGuestStateSchemaProjector.Project(document, module);

        Assert(first.SchemaVersion == 2
            && first.Strategy == "host_snapshot"
            && first.Policy == "compatible"
            && first.ContractVersion == 1,
            "unannotated scripts should publish the compatible version-one schema v2 contract");
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

    private static void ExplicitContractsRequirePersistedFields()
    {
        SemanticDocument baseline = CSharpGuestSemanticFixture.Create();
        SemanticSymbol implicitField = Field(baseline, "Implicit", "type:int32");
        SemanticDocument document = WithContract(
            baseline with { Symbols = baseline.Symbols.Append(implicitField).ToArray() },
            "explicit",
            2,
            new SemanticStateFieldContract(CSharpGuestSemanticFixture.StateFieldId, "persist", Array.Empty<string>()),
            new SemanticStateFieldContract(implicitField.Id, "implicit", Array.Empty<string>()));

        CSharpGuestStateSchema schema = CSharpGuestStateSchemaProjector.Project(document, Lower(document, 'd'));

        Assert(schema.Policy == "explicit" && schema.ContractVersion == 2,
            "explicit contracts should retain semantic policy and version");
        Assert(schema.Slots.Select(slot => slot.StableId).SequenceEqual(new[]
            { "state:type:global::Game.Script:Score" }),
            "explicit contracts should include only persisted fields");
    }

    private static void TransientConstAndReadonlyFieldsAreExcluded()
    {
        SemanticDocument baseline = CSharpGuestSemanticFixture.Create();
        SemanticSymbol transientField = Field(baseline, "Transient", "type:int32");
        SemanticSymbol constantField = Field(baseline, "Constant", "type:int32") with { IsConst = true };
        SemanticSymbol readonlyField = Field(baseline, "Readonly", "type:int32") with { IsReadonly = true };
        SemanticDocument document = WithContract(
            baseline with
            {
                Symbols = baseline.Symbols.Concat(new[] { transientField, constantField, readonlyField }).ToArray(),
            },
            "compatible",
            1,
            new SemanticStateFieldContract(CSharpGuestSemanticFixture.StateFieldId, "implicit", Array.Empty<string>()),
            new SemanticStateFieldContract(transientField.Id, "transient", Array.Empty<string>()),
            new SemanticStateFieldContract(constantField.Id, "persist", Array.Empty<string>()),
            new SemanticStateFieldContract(readonlyField.Id, "persist", Array.Empty<string>()));

        CSharpGuestStateSchema schema = CSharpGuestStateSchemaProjector.Project(document, Lower(document, 'e'));

        Assert(schema.Slots.Count == 1 && schema.Slots[0].StableId.EndsWith(":Score", StringComparison.Ordinal),
            "transient, const, and readonly fields must not enter a state schema");
    }

    private static void AliasesExpandToStableIdsAndSerializeInFixedOrder()
    {
        SemanticDocument baseline = CSharpGuestSemanticFixture.Create();
        SemanticDocument document = WithContract(
            baseline,
            "explicit",
            7,
            new SemanticStateFieldContract(
                CSharpGuestSemanticFixture.StateFieldId,
                "persist",
                new[] { "OldZ", "OldA" }));
        CSharpGuestStateSchema first = CSharpGuestStateSchemaProjector.Project(document, Lower(document, 'f'));
        CSharpGuestStateSchema second = CSharpGuestStateSchemaProjector.Project(document, Lower(document, 'f'));
        string json = Encoding.UTF8.GetString(CSharpGuestStateSchemaSerializer.Serialize(first));

        Assert(first.Slots[0].Aliases.SequenceEqual(new[]
            {
                "state:type:global::Game.Script:OldA",
                "state:type:global::Game.Script:OldZ",
            }),
            "aliases should expand to ordinal-sorted stable IDs");
        Assert(IndexOf(json, "\"schema_version\"", "\"strategy\"", "\"policy\"", "\"contract_version\"", "\"owner_type_id\"", "\"slots\"")
                && IndexOf(json, "\"stable_id\"", "\"aliases\"", "\"type_fingerprint\"", "\"offset\"", "\"size\"", "\"alignment\""),
            "schema v2 root and slot fields must serialize in the published order");
        Assert(CSharpGuestStateSchemaSerializer.Serialize(first).SequenceEqual(CSharpGuestStateSchemaSerializer.Serialize(second)),
            "alias schema serialization should remain byte-identical");
    }

    private static void AliasAndCurrentStableIdsMustNotConflict()
    {
        SemanticDocument baseline = CSharpGuestSemanticFixture.Create();
        SemanticSymbol current = Field(baseline, "Current", "type:int32");
        SemanticDocument document = WithContract(
            baseline with { Symbols = baseline.Symbols.Append(current).ToArray() },
            "compatible",
            1,
            new SemanticStateFieldContract(
                CSharpGuestSemanticFixture.StateFieldId,
                "persist",
                new[] { "Current" }),
            new SemanticStateFieldContract(current.Id, "implicit", Array.Empty<string>()));

        InvalidDataException exception = ExpectInvalidData(() =>
            CSharpGuestStateSchemaProjector.Project(document, Lower(document, '1')));

        Assert(exception.Message.Contains("ASSTATE1002", StringComparison.Ordinal),
            "alias/current stable ID conflicts should fail with the stable alias diagnostic");
    }

    private static void ExplicitUnsafePersistFailsWithAsState1005()
    {
        SemanticDocument baseline = CSharpGuestSemanticFixture.Create();
        SemanticSymbol unsafeField = Field(baseline, "Values", "type:int32[]");
        SemanticDocument document = WithContract(
            baseline with { Symbols = baseline.Symbols.Append(unsafeField).ToArray() },
            "explicit",
            2,
            new SemanticStateFieldContract(CSharpGuestSemanticFixture.StateFieldId, "persist", Array.Empty<string>()),
            new SemanticStateFieldContract(unsafeField.Id, "persist", Array.Empty<string>()));

        InvalidDataException exception = ExpectInvalidData(() =>
            CSharpGuestStateSchemaProjector.Project(document, Lower(document, '2')));

        Assert(exception.Message.Contains("ASSTATE1005", StringComparison.Ordinal),
            "explicit unsafe persisted fields must fail with ASSTATE1005");
    }

    private static void CompatibleUnsafeImplicitFieldsAreSkipped()
    {
        SemanticDocument baseline = CSharpGuestSemanticFixture.Create();
        SemanticSymbol unsafeField = Field(baseline, "Values", "type:int32[]");
        SemanticDocument document = WithContract(
            baseline with { Symbols = baseline.Symbols.Append(unsafeField).ToArray() },
            "compatible",
            1,
            new SemanticStateFieldContract(CSharpGuestSemanticFixture.StateFieldId, "implicit", Array.Empty<string>()),
            new SemanticStateFieldContract(unsafeField.Id, "implicit", Array.Empty<string>()));

        CSharpGuestStateSchema schema = CSharpGuestStateSchemaProjector.Project(document, Lower(document, '3'));

        Assert(schema.Slots.Count == 1 && schema.Slots[0].StableId.EndsWith(":Score", StringComparison.Ordinal),
            "compatible contracts should silently skip unsafe implicit fields");
    }

    private static void ContractVersionBoundsUseAsState1003()
    {
        SemanticDocument baseline = CSharpGuestSemanticFixture.Create();
        GuestModule module = Lower(baseline, '5');
        foreach (int version in new[] { 0, 65536 })
        {
            SemanticDocument document = WithContract(
                baseline,
                "compatible",
                version,
                new SemanticStateFieldContract(
                    CSharpGuestSemanticFixture.StateFieldId,
                    "implicit",
                    Array.Empty<string>()));

            InvalidDataException exception = ExpectInvalidData(() =>
                CSharpGuestStateSchemaProjector.Project(document, module));

            Assert(exception.Message.StartsWith("ASSTATE1003:", StringComparison.Ordinal),
                $"contract version {version} should fail with ASSTATE1003");
        }
    }

    private static void SourcePipelineProjectsExplicitStateSchemaV2()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using AvidScript;

            namespace Game;

            [AvidStateContract(AvidStateMode.Explicit, Version = 3)]
            public static class SourceScript
            {
                [AvidPersist, AvidStateAlias("OldScore")]
                private static int Score;

                [AvidTransient]
                private static int DebugCounter;

                private static int Implicit;

                [UnmanagedCallersOnly(EntryPoint = "guest_main")]
                public static void Main()
                {
                }
            }
            """;

        SemanticDocument document = AnalyzeSource(source, "Scripts/SourceStateSchema.cs");
        GuestModule module = Lower(document, '6');
        CSharpGuestStateSchema schema = CSharpGuestStateSchemaProjector.Project(document, module);

        Assert(schema.SchemaVersion == 2
            && schema.Policy == "explicit"
            && schema.ContractVersion == 3
            && schema.OwnerTypeId == "type:global::Game.SourceScript",
            "source pipeline should preserve the explicit state contract in schema v2");
        Assert(schema.Slots.Count == 1
            && schema.Slots[0].StableId == "state:type:global::Game.SourceScript:Score"
            && schema.Slots[0].Aliases.SequenceEqual(new[]
                { "state:type:global::Game.SourceScript:OldScore" }),
            "source pipeline should include persisted state and exclude transient or implicit fields");
    }

    private static void CompatiblePersistedUnsafeSourceFailsWithAsState1005()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using AvidScript;

            namespace Game;

            [AvidStateContract(AvidStateMode.Compatible)]
            public static class UnsafeScript
            {
                [AvidPersist]
                private static int[] Values;

                [UnmanagedCallersOnly(EntryPoint = "guest_main")]
                public static void Main()
                {
                }
            }
            """;

        SemanticDocument document = AnalyzeSource(source, "Scripts/CompatibleUnsafeState.cs");
        GuestModule module = Lower(document, '7');
        InvalidDataException exception = ExpectInvalidData(() =>
            CSharpGuestStateSchemaProjector.Project(document, module));

        Assert(exception.Message.StartsWith("ASSTATE1005:", StringComparison.Ordinal),
            "compatible persisted unsafe source fields must fail with ASSTATE1005");
    }

    private static void DelegateEventOnlyScriptDefinesStateOwner()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            [AvidStateContract(AvidStateMode.Explicit)]
            public static class EventOnlyScript
            {
                [AvidEvent(AvidEvents.OnSignal)]
                public static void HandleSignal()
                {
                }
            }
            """;

        SemanticDocument document = AnalyzeSource(
            source,
            "Scripts/DelegateEventOnlyState.cs",
            new SemanticReferenceSource(
                DelegateEventFacade,
                "generated://AvidScript.DelegateEventState.cs",
                true));
        GuestModule module = Lower(document, '8');
        CSharpGuestStateSchema schema = CSharpGuestStateSchemaProjector.Project(document, module);

        Assert(document.Callables.All(callable => callable.Export is null)
            && document.DelegateEventCallbacks.Count == 1
            && schema.OwnerTypeId == "type:global::Game.EventOnlyScript"
            && schema.Policy == "explicit"
            && schema.Slots.Count == 0,
            "delegate-event-only scripts should derive their state owner from entrypoint roots");
    }

    private static void TypeFingerprintChangesWithValueLayout()
    {
        SemanticDocument document = CSharpGuestSemanticFixture.Create();
        GuestModule module = Lower(document, '4');
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

    private static SemanticDocument WithContract(
        SemanticDocument document,
        string policy,
        int version,
        params SemanticStateFieldContract[] fields)
    {
        return document with
        {
            StateContracts = new[]
            {
                new SemanticStateContract("type:global::Game.Script", policy, version, fields),
            },
        };
    }

    private static SemanticSymbol Field(SemanticDocument document, string name, string typeId)
    {
        return new SemanticSymbol(
            $"symbol:field:global::Game.Script.{name}:{typeId[5..]}",
            "field",
            name,
            "symbol:type:global::Game.Script",
            typeId,
            $"{typeId[5..]} {name}",
            true,
            "private",
            document.Symbols[0].Span);
    }

    private static GuestModule Lower(SemanticDocument document, char hashCharacter)
    {
        return CSharpGuestLowerer.Lower(document, new string(hashCharacter, 64)).Module
            ?? throw new InvalidOperationException("Fixture should lower.");
    }

    private static SemanticDocument AnalyzeSource(
        string source,
        string sourceId,
        params SemanticReferenceSource[] additionalReferences)
    {
        string hash = FrontendAnalyzer.Analyze(source, sourceId).Source.Sha256;
        SemanticDocument document = SemanticAnalyzer.Analyze(
            source,
            sourceId,
            hash,
            new[]
            {
                new SemanticReferenceSource(
                    StateContractFacade,
                    "generated://AvidScript.StateContracts.cs"),
            }.Concat(additionalReferences).ToArray());
        Assert(document.Succeeded,
            "source semantic analysis failed: " + string.Join(", ", document.Diagnostics.Select(
                diagnostic => $"{diagnostic.Code}: {diagnostic.Message}")));
        return document;
    }

    private static InvalidDataException ExpectInvalidData(Action action)
    {
        try
        {
            action();
        }
        catch (InvalidDataException exception)
        {
            return exception;
        }

        throw new InvalidOperationException("Expected InvalidDataException.");
    }

    private static bool IndexOf(string value, params string[] fields)
    {
        int previous = -1;
        foreach (string field in fields)
        {
            int current = value.IndexOf(field, previous + 1, StringComparison.Ordinal);
            if (current <= previous)
            {
                return false;
            }

            previous = current;
        }

        return true;
    }

    private const string StateContractFacade = """
        using System;

        namespace AvidScript;

        public enum AvidStateMode
        {
            Compatible = 0,
            Explicit = 1,
        }

        [AttributeUsage(AttributeTargets.Class, Inherited = false, AllowMultiple = false)]
        public sealed class AvidStateContractAttribute : Attribute
        {
            public AvidStateContractAttribute(AvidStateMode mode = AvidStateMode.Compatible)
            {
                Mode = mode;
            }

            public AvidStateMode Mode { get; }

            public int Version { get; set; } = 1;
        }

        [AttributeUsage(AttributeTargets.Field, Inherited = false, AllowMultiple = false)]
        public sealed class AvidPersistAttribute : Attribute
        {
        }

        [AttributeUsage(AttributeTargets.Field, Inherited = false, AllowMultiple = false)]
        public sealed class AvidTransientAttribute : Attribute
        {
        }

        [AttributeUsage(AttributeTargets.Field, Inherited = false, AllowMultiple = true)]
        public sealed class AvidStateAliasAttribute : Attribute
        {
            public AvidStateAliasAttribute(string formerName)
            {
                FormerName = formerName;
            }

            public string FormerName { get; }
        }
        """;

    private const string DelegateEventFacade = """
        using System;

        namespace AvidScript;

        [AttributeUsage(AttributeTargets.Method)]
        public sealed class AvidEventAttribute : Attribute
        {
            public AvidEventAttribute(string subscriptionId)
            {
            }
        }

        [AttributeUsage(AttributeTargets.Field)]
        public sealed class AvidEventContractAttribute : Attribute
        {
            public AvidEventContractAttribute(string subscriptionId, string parameterTypes)
            {
            }
        }

        public static class AvidEvents
        {
            [AvidEventContract(
                "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                "")]
            public const string OnSignal =
                "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        }
        """;
}
