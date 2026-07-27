using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using AvidScript.CSharpFrontend;
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
        VoidFallthroughExitReturnsNormally();
        LoweringIsByteDeterministic();
        ReachabilityPrunesUnusedImports();
        BidirectionalPropertyAssignmentLowersThroughAccessor();
        PropertyAssignmentEvaluatesReceiverBeforeRightHandSide();
        CompoundPropertyAssignmentCapturesReceiverOnce();
        NaturalGameplayCallbacksSynthesizeOneRouter();
        MissingGameplayCallbacksRemainNoOp();
        ExplicitGameplayRouterConflictFailsClosed();
        ImportDispatchClassDefaultsRoundTripsAndValidates();
        InvalidImportDispatchClassFailsClosed();
        return 13;
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
        Assert(module.Imports[0].DispatchClass == "semantic",
            "semantic imports should retain the semantic dispatch class");
        Assert(module.Exports.Count == 1 && module.Exports[0].Name == "guest_main",
            "semantic exports should target lowered Guest functions");
        Assert(module.Provenance.SemanticSha256 == SemanticHash
            && module.Provenance.SourceSha256 == new string('a', 64)
            && module.Provenance.FrontendSha256 == new string('b', 64),
            "lowered module should preserve the complete provenance chain");
    }

    private static void VoidFallthroughExitReturnsNormally()
    {
        SemanticDocument document = CSharpGuestSemanticFixture.Create();
        SemanticControlFlowGraph graph = document.ControlFlowGraphs.Single();
        SemanticControlFlowEdge exitEdge = new(1, 2, "fallthrough", "regular");
        SemanticBasicBlock[] blocks = graph.Blocks
            .Select(block => block.Ordinal switch
            {
                1 => block with { Successors = new[] { exitEdge } },
                2 => block with { Predecessors = new[] { exitEdge } },
                _ => block,
            })
            .ToArray();
        document = document with
        {
            ControlFlowGraphs = new[] { graph with { Blocks = blocks } },
        };

        GuestModule module = CSharpGuestLowerer.Lower(document, SemanticHash).Module
            ?? throw new InvalidOperationException("void fallthrough input produced no Guest module");
        GuestFunction function = module.Functions.Single(item => item.ReturnTypeId == "type:void");
        GuestBasicBlock exitBlock = function.Blocks.Single(
            item => item.Id == $"block:{CSharpGuestSemanticFixture.MainMethodId}:2");

        Assert(exitBlock.Terminator.Kind == "return" && exitBlock.Terminator.ReturnValueId is null,
            "a reachable void exit block should return normally instead of trapping");
    }

    private static void LoweringIsByteDeterministic()
    {
        SemanticDocument document = CSharpGuestSemanticFixture.Create();

        GuestModule first = CSharpGuestLowerer.Lower(document, SemanticHash).Module!;
        GuestModule second = CSharpGuestLowerer.Lower(document, SemanticHash).Module!;

        Assert(GuestIrSerializer.Serialize(first).SequenceEqual(GuestIrSerializer.Serialize(second)),
            "the same semantic artifact should produce identical Guest IR bytes");
    }

    private static void ReachabilityPrunesUnusedImports()
    {
        SemanticDocument baseline = CSharpGuestSemanticFixture.Create();
        SemanticCallable main = baseline.Callables.Single(callable => callable.Export is not null);
        SemanticDocument document = baseline with
        {
            SchemaVersion = 5,
            SemanticVersion = "1.5",
            Reachability = new SemanticReachability(
                "export_roots",
                new[] { main.MethodSymbolId },
                new[] { main.MethodSymbolId },
                Array.Empty<SemanticReachableImport>()),
        };

        GuestModule module = CSharpGuestLowerer.Lower(document, SemanticHash).Module
            ?? throw new InvalidOperationException("reachable semantic input produced no Guest module");

        Assert(module.Imports.Count == 0,
            "imports outside the export-root callable closure should not enter Guest IR");
    }

    private static void ImportDispatchClassDefaultsRoundTripsAndValidates()
    {
        GuestModule module = CSharpGuestLowerer.Lower(
            CSharpGuestSemanticFixture.Create(),
            SemanticHash).Module
            ?? throw new InvalidOperationException("valid semantic input produced no Guest module");
        GuestImport legacyCompatibleImport = new(
            module.Imports[0].Id,
            module.Imports[0].Module,
            module.Imports[0].Name,
            module.Imports[0].ParameterTypeIds,
            module.Imports[0].ReturnTypeId);
        GuestModule withLegacyCompatibleImport = module with { Imports = new[] { legacyCompatibleImport } };
        string serialized = Encoding.UTF8.GetString(GuestIrSerializer.Serialize(withLegacyCompatibleImport));
        string legacySerialized = serialized.Replace(
            ",\n      \"dispatch_class\": \"semantic\"",
            string.Empty,
            StringComparison.Ordinal);
        GuestModule restored = GuestIrSerializer.Deserialize(Encoding.UTF8.GetBytes(legacySerialized));

        Assert(GuestModuleValidator.CurrentSchemaVersion == 2
            && GuestModuleValidator.CurrentIrVersion == "1.1",
            "Guest IR dispatch metadata should advance the current schema identity once");
        Assert(legacyCompatibleImport.DispatchClass == "semantic"
            && restored.Imports[0].DispatchClass == "semantic",
            "missing dispatch metadata must remain source-compatible as semantic after roundtrip");
        Assert(GuestModuleValidator.Validate(restored).Succeeded,
            "dispatch metadata roundtrip should remain independently valid");
    }

    private static void InvalidImportDispatchClassFailsClosed()
    {
        GuestModule module = CSharpGuestLowerer.Lower(
            CSharpGuestSemanticFixture.Create(),
            SemanticHash).Module
            ?? throw new InvalidOperationException("valid semantic input produced no Guest module");
        GuestImport invalid = module.Imports[0] with { DispatchClass = "unchecked" };
        GuestValidationResult result = GuestModuleValidator.Validate(module with { Imports = new[] { invalid } });

        Assert(!result.Succeeded && result.Diagnostics.Any(diagnostic => diagnostic.Code == "ASIR1011"),
            "invalid import dispatch classes should fail with the stable ASIR1011 diagnostic");
    }

    private static void BidirectionalPropertyAssignmentLowersThroughAccessor()
    {
        const string source = """
            using System.Runtime.InteropServices;

            namespace Game;

            public readonly struct ActorProxy
            {
                public readonly int Slot;
                public readonly int Generation;

                public ActorProxy(int slot, int generation)
                {
                    Slot = slot;
                    Generation = generation;
                }

                public float CustomTimeDilation
                {
                    set => Native.SetCustomTimeDilation(Slot, Generation, value);
                }
            }

            internal static class Native
            {
                [DllImport("avidscript", EntryPoint = "avid_property_set_custom_time_dilation")]
                internal static extern int SetCustomTimeDilation(int slot, int generation, float value);
            }

            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static void OnBeginPlay(int slot, int generation)
                {
                    ActorProxy actor = new(slot, generation);
                    actor.CustomTimeDilation = 2.5f;
                }
            }
            """;
        SemanticDocument document = AnalyzeGameplaySource(
            source,
            "Scripts/BidirectionalPropertyAssignment.cs");
        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(document, SemanticHash);
        GuestModule module = result.Module
            ?? throw new InvalidOperationException("property assignment produced no Guest module");

        Assert(result.Succeeded, "natural C# property assignment should lower");
        GuestFunction entry = module.Functions.Single(function =>
            function.Id == module.Exports.Single(item => item.Name == "avid_on_begin_play").FunctionId);
        GuestFunction setter = module.Functions.Single(function =>
            function.Id.Contains("set_CustomTimeDilation", StringComparison.Ordinal));
        GuestImport hostImport = module.Imports.Single(item =>
            item.Name == "avid_property_set_custom_time_dilation");

        Assert(entry.Blocks.SelectMany(block => block.Instructions).Any(instruction =>
                instruction.Op == "call" && instruction.TargetId == setter.Id),
            "property assignment should call the generated setter accessor");
        Assert(setter.Blocks.SelectMany(block => block.Instructions).Any(instruction =>
                instruction.Op == "call" && instruction.TargetId == hostImport.Id),
            "generated setter accessor should call its authorized host import");
        Assert(GuestModuleValidator.Validate(module).Succeeded,
            "bidirectional property Guest IR should pass independent validation");
    }

    private static void PropertyAssignmentEvaluatesReceiverBeforeRightHandSide()
    {
        GuestFunction entry = LowerPropertyEvaluationSource(
            "AcquireActor().Value = ComputeValue();",
            "avid_on_begin_play");
        GuestInstruction[] calls = entry.Blocks
            .SelectMany(block => block.Instructions)
            .Where(instruction => instruction.Op == "call")
            .ToArray();

        int receiverIndex = FindSingleCall(calls, "AcquireActor(");
        int rightHandSideIndex = FindSingleCall(calls, "ComputeValue(");
        int setterIndex = FindSingleCall(calls, "set_Value(");
        Assert(receiverIndex < rightHandSideIndex && rightHandSideIndex < setterIndex,
            "property assignment must evaluate receiver before RHS and invoke setter last");
        Assert(calls[receiverIndex].ResultId is not null
            && calls[setterIndex].OperandIds.Count > 0
            && calls[setterIndex].OperandIds[0] == calls[receiverIndex].ResultId,
            "property setter must receive the captured receiver register");
    }

    private static void CompoundPropertyAssignmentCapturesReceiverOnce()
    {
        GuestFunction entry = LowerPropertyEvaluationSource(
            "AcquireActor().Value += ComputeValue();",
            "avid_on_tick");
        GuestInstruction[] calls = entry.Blocks
            .SelectMany(block => block.Instructions)
            .Where(instruction => instruction.Op == "call")
            .ToArray();

        int receiverIndex = FindSingleCall(calls, "AcquireActor(");
        int getterIndex = FindSingleCall(calls, "get_Value(");
        int rightHandSideIndex = FindSingleCall(calls, "ComputeValue(");
        int setterIndex = FindSingleCall(calls, "set_Value(");
        GuestInstruction getter = calls[getterIndex];
        GuestInstruction setter = calls[setterIndex];

        Assert(receiverIndex < getterIndex
            && getterIndex < rightHandSideIndex
            && rightHandSideIndex < setterIndex,
            "compound property assignment must evaluate receiver, getter, RHS, then setter");
        Assert(getter.OperandIds.Count > 0
            && setter.OperandIds.Count > 0
            && calls[receiverIndex].ResultId is not null
            && getter.OperandIds[0] == calls[receiverIndex].ResultId
            && setter.OperandIds[0] == calls[receiverIndex].ResultId,
            "compound property getter and setter must share the captured receiver register");
    }

    private static GuestFunction LowerPropertyEvaluationSource(string operation, string exportName)
    {
        string source = $$"""
            using System.Runtime.InteropServices;

            namespace Game;

            public readonly struct ActorProxy
            {
                public float Value
                {
                    get => 1.0f;
                    set { }
                }
            }

            public static class Script
            {
                private static ActorProxy AcquireActor() => default;
                private static float ComputeValue() => 2.0f;

                [UnmanagedCallersOnly(EntryPoint = "{{exportName}}")]
                public static void Run()
                {
                    {{operation}}
                }
            }
            """;
        SemanticDocument document = AnalyzeGameplaySource(
            source,
            $"Scripts/{exportName}.PropertyEvaluation.cs");
        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(document, SemanticHash);
        GuestModule module = result.Module
            ?? throw new InvalidOperationException($"{exportName} property evaluation produced no Guest module");

        Assert(result.Succeeded, $"{exportName} property evaluation should lower");
        return module.Functions.Single(function =>
            function.Id == module.Exports.Single(item => item.Name == exportName).FunctionId);
    }

    private static int FindSingleCall(IReadOnlyList<GuestInstruction> calls, string methodName)
    {
        int[] matches = calls
            .Select((instruction, index) => (instruction, index))
            .Where(item => item.instruction.TargetId?.Contains(methodName, StringComparison.Ordinal) == true)
            .Select(item => item.index)
            .ToArray();
        Assert(matches.Length == 1,
            $"expected exactly one call to '{methodName}', found {matches.Length}");
        return matches[0];
    }

    private static void NaturalGameplayCallbacksSynthesizeOneRouter()
    {
        SemanticDocument document = AnalyzeGameplaySource(AllGameplayCallbacksSource, "Scripts/AllGameplayCallbacks.cs");

        GuestModule module = CSharpGuestLowerer.Lower(document, SemanticHash).Module
            ?? throw new InvalidOperationException("natural gameplay callbacks produced no Guest module");
        GuestExport gameplayExport = module.Exports.Single(item => item.Name == "avid_on_gameplay_event");
        GuestFunction router = module.Functions.Single(item => item.Id == gameplayExport.FunctionId);
        GuestInstruction[] instructions = router.Blocks.SelectMany(block => block.Instructions).ToArray();

        Assert(router.Parameters.Select(parameter => parameter.TypeId).SequenceEqual(new[]
            {
                "type:int32", "type:int32", "type:int32", "type:int32", "type:int32",
                "type:float32", "type:float32", "type:float32",
            }),
            "synthetic gameplay router should preserve the stable runtime ABI");
        Assert(router.Blocks.Count(block => block.Terminator.Kind == "branch_if") == 4,
            "synthetic gameplay router should branch once for every declared event type");
        Assert(instructions.Count(instruction => instruction.Op == "call") == 4,
            "synthetic gameplay router should call every declared natural callback once");
        Assert(instructions.Any(instruction => instruction.Op == "field_store"),
            "synthetic gameplay router should initialize typed aggregate payloads");
        foreach (SemanticGameplayEventCallback callback in document.GameplayEventCallbacks)
        {
            Assert(instructions.Any(instruction => instruction.Op == "call"
                && instruction.TargetId == "function:" + callback.MethodSymbolId),
                $"synthetic gameplay router should call {callback.Name}");
        }

        Assert(GuestModuleValidator.Validate(module).Succeeded,
            "synthetic gameplay router should pass independent Guest IR validation");
    }

    private static void MissingGameplayCallbacksRemainNoOp()
    {
        const string source = """
            namespace AvidScript;

            public readonly struct FVector
            {
                public readonly float X;
                public readonly float Y;
                public readonly float Z;
            }

            public readonly struct InputEvent
            {
                public readonly int ActionId;
                public readonly int TriggerEvent;
                public readonly FVector Value;
            }

            public static class Script
            {
                public static void OnInput(InputEvent input) { }
            }
            """;
        SemanticDocument document = AnalyzeGameplaySource(source, "Scripts/InputOnlyGameplayCallback.cs");

        GuestModule module = CSharpGuestLowerer.Lower(document, SemanticHash).Module
            ?? throw new InvalidOperationException("input-only callback produced no Guest module");
        GuestFunction router = module.Functions.Single(function =>
            function.Id == module.Exports.Single(item => item.Name == "avid_on_gameplay_event").FunctionId);
        GuestInstruction[] instructions = router.Blocks.SelectMany(block => block.Instructions).ToArray();

        Assert(router.Blocks.Count(block => block.Terminator.Kind == "branch_if") == 1,
            "missing natural callbacks should not generate dead event branches");
        Assert(instructions.Count(instruction => instruction.Op == "call") == 1,
            "missing natural callbacks should remain no-op routes");
        Assert(instructions.Single(instruction => instruction.Op == "call").TargetId ==
            "function:" + document.GameplayEventCallbacks.Single().MethodSymbolId,
            "input-only router should call only OnInput");
    }

    private static void ExplicitGameplayRouterConflictFailsClosed()
    {
        const string source = """
            using System.Runtime.InteropServices;

            namespace AvidScript;

            public readonly struct InputEvent { }

            public static class Script
            {
                public static void OnInput(InputEvent input) { }

                [UnmanagedCallersOnly(EntryPoint = "avid_on_gameplay_event")]
                public static void OnGameplayEvent(
                    int eventType,
                    int primaryId,
                    int secondaryId,
                    int objectSlot,
                    int objectGeneration,
                    float x,
                    float y,
                    float z) { }
            }
            """;
        SemanticDocument document = AnalyzeGameplaySource(source, "Scripts/ConflictingGameplayRouter.cs");

        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(document, SemanticHash);

        Assert(!result.Succeeded && result.Module is null,
            "an explicit raw gameplay router should conflict with compiler synthesis");
        Assert(result.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCG1007"),
            "an explicit raw gameplay router conflict should report ASCG1007");
    }

    private static SemanticDocument AnalyzeGameplaySource(string source, string sourceId)
    {
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        SemanticDocument document = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);
        Assert(document.Succeeded, $"{sourceId} should produce a valid semantic artifact");
        return document;
    }

    private const string AllGameplayCallbacksSource = """
        namespace AvidScript;

        public readonly struct AActor
        {
            public readonly int Slot;
            public readonly int Generation;
        }

        public readonly struct FVector
        {
            public readonly float X;
            public readonly float Y;
            public readonly float Z;
        }

        public readonly struct InputEvent
        {
            public readonly int ActionId;
            public readonly int TriggerEvent;
            public readonly FVector Value;
        }

        public static class Script
        {
            public static void OnBeginOverlap(AActor otherActor, FVector location) { }
            public static void OnEndOverlap(AActor otherActor, FVector location) { }
            public static void OnHit(AActor otherActor, FVector normalImpulse) { }
            public static void OnInput(InputEvent input) { }
        }
        """;

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
