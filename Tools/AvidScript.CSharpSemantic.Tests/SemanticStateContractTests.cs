using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpSemantic;

internal static class SemanticStateContractTests
{
    public static int Run()
    {
        CompatibleContractsPreserveImplicitPersistAndTransientFields();
        UnannotatedContractsDefaultToCompatibleVersionOne();
        ExplicitContractsRequirePersistedFields();
        CanonicalFacadeAttributesAreRequired();
        DuplicateAndConflictingAttributesFailClosed();
        InvalidAliasesAndVersionsFailClosed();
        SubscriptionCapabilitiesMustBeTransient();
        StateContractSerializationIsDeterministic();
        return 8;
    }

    private static void CompatibleContractsPreserveImplicitPersistAndTransientFields()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            [AvidStateContract(AvidStateMode.Compatible, Version = 7)]
            public static class CompatibleScript
            {
                private const int Constant = 1;
                private static readonly int Readonly = 2;
                private static int Implicit;
                [AvidPersist, AvidStateAlias("OldPersist")]
                private static int Persisted;
                [AvidTransient]
                private static int Temporary;
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/CompatibleState.cs");
        SemanticStateContract contract = FindContract(document, "global::Game.CompatibleScript");

        Assert(document.Succeeded, "compatible state contract should analyze successfully");
        Assert(document.Types.Any(type => type.Id == contract.OwnerTypeId),
            "state contract owner should use an identity from the semantic type registry");
        Assert(contract.Policy == "compatible" && contract.Version == 7,
            "compatible contract should retain its policy and version");
        AssertField(contract, "Persisted", "persist", "OldPersist");
        AssertField(contract, "Temporary", "transient");
        AssertField(contract, "Implicit", "implicit");
        SemanticSymbol constant = FindSymbol(document, "Constant");
        SemanticSymbol readOnly = FindSymbol(document, "Readonly");
        Assert(constant.IsConst, "const fields should retain is_const");
        Assert(readOnly.IsReadonly, "readonly fields should retain is_readonly");
    }

    private static void UnannotatedContractsDefaultToCompatibleVersionOne()
    {
        const string source = """
            namespace Game;

            public static class LegacyScript
            {
                private static int Score;
            }
            """;

        SemanticStateContract contract = FindContract(
            Analyze(source, "Scripts/LegacyState.cs"),
            "global::Game.LegacyScript");

        Assert(contract.Policy == "compatible" && contract.Version == 1,
            "unannotated classes should default to compatible policy and version 1");
        AssertField(contract, "Score", "implicit");
    }

    private static void ExplicitContractsRequirePersistedFields()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            [AvidStateContract(AvidStateMode.Explicit, Version = 2)]
            public static class ExplicitScript
            {
                private static int Implicit;
                [AvidPersist, AvidStateAlias("OldScore")]
                private static int Score;
                [AvidTransient]
                private static int DebugCounter;
            }
            """;

        SemanticStateContract contract = FindContract(
            Analyze(source, "Scripts/ExplicitState.cs"),
            "global::Game.ExplicitScript");

        Assert(contract.Policy == "explicit" && contract.Version == 2,
            "explicit contract should retain its policy and version");
        AssertField(contract, "Implicit", "implicit");
        AssertField(contract, "Score", "persist", "OldScore");
        AssertField(contract, "DebugCounter", "transient");
    }

    private static void CanonicalFacadeAttributesAreRequired()
    {
        const string source = """
            using System;
            using Spoof;

            namespace Spoof
            {
                public sealed class AvidPersistAttribute : Attribute
                {
                }
            }

            namespace Game
            {
                public static class SpoofedScript
                {
                    [AvidPersist]
                    private static int Value;
                }
            }
            """;

        SemanticStateContract contract = FindContract(
            Analyze(source, "Scripts/SpoofedState.cs"),
            "global::Game.SpoofedScript");

        AssertField(contract, "Value", "implicit");
    }

    private static void DuplicateAndConflictingAttributesFailClosed()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            [AvidStateContract]
            [AvidStateContract]
            public static class InvalidAttributesScript
            {
                [AvidPersist]
                [AvidPersist]
                [AvidTransient]
                private static int Value;
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/InvalidAttributes.cs");

        Assert(!document.Succeeded, "duplicate or conflicting state attributes should fail semantic analysis");
        Assert(document.Diagnostics.Count(diagnostic => diagnostic.Code == "ASSTATE1001") >= 2,
            "duplicate contract and field attribute conflicts should use ASSTATE1001");
    }

    private static void InvalidAliasesAndVersionsFailClosed()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            [AvidStateContract(Version = 0)]
            public static class LowVersionScript
            {
                [AvidPersist, AvidStateAlias("invalid alias"), AvidStateAlias("Other"), AvidStateAlias("Other")]
                private static int Value;
                private static int Other;
                [AvidTransient, AvidStateAlias("OldTemporary")]
                private static int Temporary;
            }

            [AvidStateContract(Version = 65536)]
            public static class HighVersionScript
            {
                private static int Value;
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/InvalidStateMetadata.cs");

        Assert(!document.Succeeded, "invalid aliases and versions should fail semantic analysis");
        Assert(document.Diagnostics.Count(diagnostic => diagnostic.Code == "ASSTATE1002") >= 2,
            "invalid, duplicate, or conflicting aliases should use ASSTATE1002");
        Assert(document.Diagnostics.Count(diagnostic => diagnostic.Code == "ASSTATE1003") == 2,
            "out-of-range versions should use ASSTATE1003");
        Assert(document.Diagnostics.Any(diagnostic => diagnostic.Code == "ASSTATE1004"),
            "aliases on non-participating fields should use ASSTATE1004");
    }

    private static void StateContractSerializationIsDeterministic()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            [AvidStateContract(AvidStateMode.Explicit, Version = 3)]
            public static class DeterministicScript
            {
                [AvidPersist, AvidStateAlias("OldB"), AvidStateAlias("OldA")]
                private static int Score;
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/DeterministicState.cs");
        AssertField(FindContract(document, "global::Game.DeterministicScript"), "Score", "persist", "OldA", "OldB");
        byte[] first = SemanticSerializer.Serialize(document);
        byte[] second = SemanticSerializer.Serialize(Analyze(source, "Scripts/DeterministicState.cs"));

        Assert(first.SequenceEqual(second), "state contract serialization should be byte-for-byte deterministic");
    }

    private static void SubscriptionCapabilitiesMustBeTransient()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            public static class CompatibleScript
            {
                private static AvidSubscription InvalidImplicit;
                [AvidTransient]
                private static AvidSubscription ValidTransient;
            }

            [AvidStateContract(AvidStateMode.Explicit)]
            public static class ExplicitScript
            {
                [AvidPersist]
                private static AvidSubscription InvalidPersisted;
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/SubscriptionState.cs");

        Assert(!document.Succeeded
            && document.Diagnostics.Count(diagnostic => diagnostic.Code == "ASSTATE1005") == 2,
            "implicit-compatible and explicitly persisted subscription tokens should fail closed");
        SemanticStateContract compatible = FindContract(
            document,
            "global::Game.CompatibleScript");
        AssertField(compatible, "ValidTransient", "transient");
    }

    private static SemanticDocument Analyze(string source, string sourceId)
    {
        string hash = FrontendAnalyzer.Analyze(source, sourceId).Source.Sha256;
        return SemanticAnalyzer.Analyze(
            source,
            sourceId,
            hash,
            new[] { new SemanticReferenceSource(StateContractFacade, "generated://AvidScript.StateContracts.cs") });
    }

    private static SemanticStateContract FindContract(SemanticDocument document, string canonicalTypeName)
    {
        string ownerTypeId = "type:" + canonicalTypeName;
        SemanticStateContract? contract = document.StateContracts.SingleOrDefault(item =>
            item.OwnerTypeId == ownerTypeId);
        Assert(contract is not null, $"state contract owner should use semantic type identity {ownerTypeId}");
        return contract!;
    }

    private static SemanticSymbol FindSymbol(SemanticDocument document, string name)
    {
        return document.Symbols.Single(symbol => symbol.Kind == "field" && symbol.Name == name);
    }

    private static void AssertField(
        SemanticStateContract contract,
        string name,
        string disposition,
        params string[] aliases)
    {
        SemanticStateFieldContract field = contract.Fields.Single(item => item.SymbolId.Contains(
            "." + name + ":",
            StringComparison.Ordinal));
        Assert(field.Disposition == disposition, $"{name} should have {disposition} disposition");
        Assert(field.Aliases.SequenceEqual(aliases), $"{name} aliases should be deterministic");
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
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

        public readonly struct AvidSubscription
        {
            private readonly long Token;
        }
        """;
}
