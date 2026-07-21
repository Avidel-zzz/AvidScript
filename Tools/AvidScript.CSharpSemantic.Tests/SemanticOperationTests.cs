using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpSemantic;

internal static class SemanticOperationTests
{
    public static int Run()
    {
        CoreOperationsRetainTypesSymbolsConstantsAndConversions();
        ActorLifecycleOperationsBindCompleteSymbolIds();
        AccessorBodiesAreProjectedAsMethods();
        ExpressionBodiedPropertiesProjectGetterOperations();
        RefOverloadsRetainDistinctCompleteSymbolIds();
        CompoundAndArgumentConversionsRetainRoslynSemantics();
        OperatorKindsRetainLoweringSemantics();
        ExpressionBodiedIndexerOverloadsRemainDistinct();
        GenericMethodArityRemainsDistinct();
        OperationSchemaVersionIsExplicit();
        ConversionOperationsRetainCastSemantics();
        ControlFlowOperationsAreSupportedWithCfgProjection();
        UnsupportedOperationsArePreservedAndFailClosed();
        return 13;
    }

    private static void CoreOperationsRetainTypesSymbolsConstantsAndConversions()
    {
        const string source = """
            namespace Game;

            public readonly struct FVector
            {
                public float X { get; }
                public FVector(float x) { X = x; }
            }

            public sealed class Script
            {
                private float Speed;
                private float Bias { get; } = 1.0f;

                public float Tick(float deltaSeconds)
                {
                    float local = -deltaSeconds;
                    Speed += local;
                    Speed = Speed + (int)Bias;
                    return Helper(new FVector(120.0f * deltaSeconds));
                }

                private static float Helper(FVector value) => value.X;
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/CoreOperations.cs");

        Assert(document.Succeeded, "supported core operations should pass semantic analysis");
        SemanticMethodBody tick = document.Methods.Single(method =>
            method.MethodSymbolId == "symbol:method:global::Game.Script.Tick(float32):float32");
        IReadOnlyList<SemanticOperation> operations = Flatten(tick.Root);

        Assert(HasOperation(operations, "local_reference", "type:float32", ".local:"),
            "local references should retain local symbol and float32 type ids");
        Assert(HasOperation(operations, "field_reference", "type:float32", ".Speed:"),
            "field references should retain field symbol and float32 type ids");
        Assert(HasOperation(operations, "parameter_reference", "type:float32", ":deltaSeconds:"),
            "parameter references should retain parameter symbol and float32 type ids");
        Assert(operations.Any(operation => operation.Kind == "assignment"),
            "simple assignment should be projected");
        Assert(operations.Any(operation => operation.Kind == "compound_assignment"),
            "compound assignment should be projected");
        Assert(operations.Any(operation => operation.Kind == "object_creation" &&
            operation.SymbolId == "symbol:method:global::Game.FVector..ctor(float32):void"),
            "object creation should bind its complete constructor symbol id");
        Assert(operations.Any(operation => operation.Kind == "property_reference" &&
            operation.SymbolId == "symbol:property:global::Game.Script.Bias:float32"),
            "property references should bind complete property symbol ids");
        Assert(operations.Any(operation => operation.Kind == "invocation" &&
            operation.SymbolId == "symbol:method:global::Game.Script.Helper(global::Game.FVector):float32"),
            "invocations should bind complete target method symbol ids");
        Assert(operations.Any(operation => operation.Kind == "return"), "returns should be projected");
        Assert(operations.Any(operation => operation.Kind == "binary"), "binary operations should be projected");
        Assert(operations.Any(operation => operation.Kind == "unary"), "unary operations should be projected");
        Assert(operations.Any(operation => operation.Kind == "literal" &&
            operation.Constant is { Kind: "float32", Value: "120" }),
            "constants should use deterministic canonical text");
        Assert(operations.Any(operation => operation.Kind == "conversion" &&
            operation.Conversion is { Exists: true, IsNumeric: true }),
            "numeric conversions should retain Roslyn conversion semantics");
    }

    private static void ActorLifecycleOperationsBindCompleteSymbolIds()
    {
        string source = File.ReadAllText(Path.Combine(
            Directory.GetCurrentDirectory(),
            "Samples",
            "CSharp",
            "ActorLifecycle",
            "ActorLifecycleScript.cs"));
        SemanticDocument document = Analyze(source, "Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs");

        Assert(document.Succeeded, "ActorLifecycle operations should be supported");
        IReadOnlyList<SemanticOperation> operations = document.Methods
            .SelectMany(method => Flatten(method.Root))
            .ToArray();
        Assert(operations.Any(operation => operation.Kind == "object_creation" &&
            operation.SymbolId == "symbol:method:global::AvidScript.FVector..ctor(float32,float32,float32):void"),
            "FVector construction should bind its complete constructor symbol id");
        Assert(operations.Any(operation => operation.Kind == "binary" &&
            operation.SymbolId == "symbol:method:global::AvidScript.FVector.op_Addition(global::AvidScript.FVector,global::AvidScript.FVector):global::AvidScript.FVector"),
            "FVector addition should bind its complete operator symbol id");
        Assert(operations.Any(operation => operation.Kind == "property_reference" &&
            operation.SymbolId == "symbol:property:global::AvidScript.UE.Self:global::AvidScript.AActor"),
            "UE.Self should bind its complete property symbol id");
        Assert(operations.Any(operation => operation.Kind == "invocation" &&
            operation.SymbolId == "symbol:method:global::AvidScript.AActor.SetActorLocation(global::AvidScript.FVector):bool"),
            "AActor calls should bind their complete method symbol ids");
    }

    private static void CompoundAndArgumentConversionsRetainRoslynSemantics()
    {
        const string source = """
            namespace Game;
            public sealed class Script
            {
                private byte Value;
                private static void Consume(float amount) { }

                public void Tick(int amount)
                {
                    Value += 1;
                    Consume(amount);
                }
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/OperationConversions.cs");
        SemanticMethodBody tick = document.Methods.Single(method =>
            method.MethodSymbolId == "symbol:method:global::Game.Script.Tick(int32):void");
        IReadOnlyList<SemanticOperation> operations = Flatten(tick.Root);

        SemanticOperation compound = operations.Single(operation => operation.Kind == "compound_assignment");
        Assert(compound.InputConversion is { Exists: true, IsNumeric: true },
            "compound assignment should retain Roslyn's target-to-operator conversion");
        Assert(compound.OutputConversion is { Exists: true, IsNumeric: true },
            "compound assignment should retain Roslyn's operator-to-target conversion");

        const string parameterId =
            "symbol:parameter:symbol:method:global::Game.Script.Consume(float32):void:0:amount:float32";
        SemanticOperation argument = operations.Single(operation =>
            operation.Kind == "argument" && operation.SymbolId == parameterId);
        Assert(argument.InputConversion is { Exists: true, IsIdentity: true },
            "arguments should retain Roslyn's argument-level conversion contract");
        Assert(argument.Children.Any(child => child.Kind == "conversion" &&
            child.Conversion is { Exists: true, IsNumeric: true }),
            "argument values should retain Roslyn's implicit conversion to the selected parameter type");
    }
    private static void OperatorKindsRetainLoweringSemantics()
    {
        const string source = """
            namespace Game;
            public sealed class Script
            {
                public int Evaluate(int value)
                {
                    int added = value + 1;
                    int subtracted = value - 1;
                    int negated = -value;
                    value++;
                    return added + subtracted + negated + value;
                }
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/Operators.cs");
        SemanticMethodBody evaluate = document.Methods.Single(method =>
            method.MethodSymbolId == "symbol:method:global::Game.Script.Evaluate(int32):int32");
        IReadOnlyList<SemanticOperation> operations = Flatten(evaluate.Root);

        Assert(operations.Any(operation => operation.Kind == "binary" && operation.OperatorKind == "add"),
            "binary addition should retain a stable operator kind");
        Assert(operations.Any(operation => operation.Kind == "binary" && operation.OperatorKind == "subtract"),
            "binary subtraction should remain distinguishable from addition");
        Assert(operations.Any(operation => operation.Kind == "unary" && operation.OperatorKind == "negate"),
            "unary negation should retain a stable operator kind");
        Assert(operations.Any(operation => operation.Kind == "increment_or_decrement" &&
            operation.OperatorKind == "increment" && operation.IsPostfix),
            "postfix increment should retain operator direction and postfix semantics");
    }

    private static void ExpressionBodiedIndexerOverloadsRemainDistinct()
    {
        const string source = """
            namespace Game;
            public sealed class Script
            {
                public int this[int index] => index;
                public float this[float index] => index;
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/Indexers.cs");

        const string intIndexer = "symbol:property:global::Game.Script.this[int32]:int32";
        const string floatIndexer = "symbol:property:global::Game.Script.this[float32]:float32";
        Assert(document.Symbols.Any(symbol => symbol.Id == intIndexer),
            "int indexer should retain parameter types in its property id");
        Assert(document.Symbols.Any(symbol => symbol.Id == floatIndexer),
            "overloaded float indexer should retain a distinct property id");
        Assert(document.Methods.Any(method =>
            method.MethodSymbolId == "symbol:method:global::Game.Script.get_Item(int32):int32"),
            "expression-bodied int indexer getter should be projected");
        Assert(document.Methods.Any(method =>
            method.MethodSymbolId == "symbol:method:global::Game.Script.get_Item(float32):float32"),
            "expression-bodied float indexer getter should be projected");
    }

    private static void GenericMethodArityRemainsDistinct()
    {
        const string source = """
            namespace Game;
            public static class Script
            {
                public static int Create() => 1;
                public static void Create<T>() { }
                public static void Tick()
                {
                    Create<int>();
                    Create<float>();
                }
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/GenericArity.cs");

        const string nonGenericId = "symbol:method:global::Game.Script.Create():int32";
        const string genericId = "symbol:method:global::Game.Script.Create`1():void";
        Assert(document.Symbols.Any(symbol => symbol.Id == nonGenericId),
            "non-generic method should retain its ordinary stable id");
        Assert(document.Symbols.Any(symbol => symbol.Id == genericId),
            "generic method should retain generic arity in its stable id");
        SemanticMethodBody tick = document.Methods.Single(method =>
            method.MethodSymbolId == "symbol:method:global::Game.Script.Tick():void");
        IReadOnlyList<SemanticOperation> invocations = Flatten(tick.Root)
            .Where(operation => operation.Kind == "invocation" && operation.SymbolId == genericId)
            .ToArray();
        Assert(invocations.Count == 2, "constructed generic calls should bind the original definition id");
        Assert(invocations.Any(operation => operation.TypeArgumentIds.SequenceEqual(new[] { "type:int32" })),
            "Create<int> should retain its concrete type argument id");
        Assert(invocations.Any(operation => operation.TypeArgumentIds.SequenceEqual(new[] { "type:float32" })),
            "Create<float> should remain distinguishable from Create<int>");
    }

    private static void OperationSchemaVersionIsExplicit()
    {
        const string source = "class Script { int Main() => 0; }";
        SemanticDocument document = Analyze(source, "Scripts/SchemaVersion.cs");

        Assert(document.SchemaVersion == 7, "state contract artifacts should use semantic schema v7");
        Assert(document.SemanticVersion == "1.7", "state contract artifacts should advertise semantic version 1.7");
    }
    private static void ConversionOperationsRetainCastSemantics()
    {
        const string source = """
            public sealed class Script
            {
                public int Checked(long value) => checked((int)value);
                public string? TryCast(object value) => value as string;
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/CastSemantics.cs");
        IReadOnlyList<SemanticOperation> operations = document.Methods
            .SelectMany(method => Flatten(method.Root))
            .Where(operation => operation.Kind == "conversion")
            .ToArray();

        Assert(operations.Any(operation => operation.TypeId == "type:int32" && operation.IsChecked),
            "checked numeric casts should retain Roslyn's checked semantics");
        Assert(operations.Any(operation => operation.TypeId == "type:string" && operation.IsTryCast),
            "as conversions should remain distinguishable from ordinary reference casts");
    }
    private static void ControlFlowOperationsAreSupportedWithCfgProjection()
    {
        const string source = """
            public sealed class Script
            {
                public void Tick(bool active)
                {
                    if (active) { }
                    while (active) { break; }
                }
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/ControlFlowGate.cs");
        IReadOnlyList<SemanticOperation> operations = document.Methods
            .SelectMany(method => Flatten(method.Root))
            .ToArray();

        Assert(document.Succeeded, "supported control-flow operations should pass once CFG projection exists");
        Assert(operations.Any(operation => operation.Kind == "conditional" && operation.IsSupported),
            "if operations should become supported with stable CFG projection");
        Assert(operations.Any(operation => operation.Kind == "loop" && operation.IsSupported),
            "loop operations should become supported with stable CFG projection");
        Assert(operations.Any(operation => operation.Kind == "branch" && operation.IsSupported),
            "branch operations should become supported with stable CFG projection");
        Assert(document.ControlFlowGraphs.Any(graph =>
            graph.MethodSymbolId == "symbol:method:global::Script.Tick(bool):void"),
            "supported control flow should expose a CFG for the owning method");
    }
    private static void UnsupportedOperationsArePreservedAndFailClosed()
    {
        const string source = """
            public sealed class Script
            {
                private readonly object Sync = new object();
                public void Tick()
                {
                    lock (Sync) { }
                }
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/UnsupportedLock.cs");

        Assert(!document.Succeeded, "unsupported operations should fail semantic analysis");
        SemanticOperation lockOperation = document.Methods
            .SelectMany(method => Flatten(method.Root))
            .Single(operation => operation.Kind == "lock");
        Assert(!lockOperation.IsSupported, "unsupported operations should remain visible in the operation tree");
        Assert(lockOperation.Span.Length > 0, "unsupported operations should retain their UTF-16 span");
        Assert(document.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS2001" && diagnostic.Severity == "error"),
            "unsupported operations should emit a stable ASCS2xxx error");
    }

    private static void AccessorBodiesAreProjectedAsMethods()
    {
        const string source = """
            namespace Game;
            public sealed class Script
            {
                public float Bias
                {
                    get { return 1.0f; }
                }
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/Accessor.cs");

        const string getterId = "symbol:method:global::Game.Script.get_Bias():float32";
        Assert(document.Symbols.Any(symbol => symbol.Id == getterId),
            "property accessors should be present in the stable symbol table");
        SemanticMethodBody getter = document.Methods.Single(method => method.MethodSymbolId == getterId);
        Assert(Flatten(getter.Root).Any(operation => operation.Kind == "return"),
            "property getter bodies should be projected as executable methods");
    }

    private static void ExpressionBodiedPropertiesProjectGetterOperations()
    {
        const string source = """
            namespace Game;
            public static class Script
            {
                public static float Bias => 1.0f;
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/ExpressionProperty.cs");

        const string getterId = "symbol:method:global::Game.Script.get_Bias():float32";
        Assert(document.Symbols.Any(symbol => symbol.Id == getterId),
            "expression-bodied property getters should be present in the stable symbol table");
        SemanticMethodBody getter = document.Methods.Single(method => method.MethodSymbolId == getterId);
        Assert(Flatten(getter.Root).Any(operation =>
            operation.Kind == "literal" && operation.Constant is { Kind: "float32", Value: "1" }),
            "expression-bodied getter operations should be projected");
    }
    private static void RefOverloadsRetainDistinctCompleteSymbolIds()
    {
        const string source = """
            namespace Game;
            public sealed class Script
            {
                public int Pick(int value) => value;
                public int Pick(ref int value) => value;
                public int Tick(int value) => Pick(ref value);
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/RefOverloads.cs");

        const string valueOverload = "symbol:method:global::Game.Script.Pick(int32):int32";
        const string refOverload = "symbol:method:global::Game.Script.Pick(ref int32):int32";
        Assert(document.Symbols.Any(symbol => symbol.Id == valueOverload),
            "value overload should retain its complete signature");
        Assert(document.Symbols.Any(symbol => symbol.Id == refOverload),
            "ref overload should retain ref-kind in its complete signature");
        SemanticMethodBody tick = document.Methods.Single(method =>
            method.MethodSymbolId == "symbol:method:global::Game.Script.Tick(int32):int32");
        Assert(Flatten(tick.Root).Any(operation => operation.Kind == "invocation" && operation.SymbolId == refOverload),
            "overload calls should bind Roslyn's selected complete symbol id");
    }
    private static SemanticDocument Analyze(string source, string sourceId)
    {
        string hash = FrontendAnalyzer.Analyze(source, sourceId).Source.Sha256;
        return SemanticAnalyzer.Analyze(source, sourceId, hash);
    }

    private static IReadOnlyList<SemanticOperation> Flatten(SemanticOperation root)
    {
        List<SemanticOperation> operations = new();
        Visit(root, operations);
        return operations;
    }

    private static void Visit(SemanticOperation operation, ICollection<SemanticOperation> operations)
    {
        operations.Add(operation);
        foreach (SemanticOperation child in operation.Children)
        {
            Visit(child, operations);
        }
    }

    private static bool HasOperation(
        IEnumerable<SemanticOperation> operations,
        string kind,
        string typeId,
        string symbolIdFragment)
    {
        return operations.Any(operation =>
            operation.Kind == kind &&
            operation.TypeId == typeId &&
            operation.SymbolId?.Contains(symbolIdFragment, StringComparison.Ordinal) == true);
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
