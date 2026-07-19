using System;
using System.Collections.Generic;
using AvidScript.CSharpSemantic;

internal static class CSharpGuestSemanticFixture
{
    public const string MainMethodId = "symbol:method:global::Game.Script.Main():void";
    public const string HostMethodId = "symbol:method:global::Game.Host.Add(int32,int32):int32";
    public const string StateFieldId = "symbol:field:global::Game.Script.Score:int32";

    public static SemanticDocument Create(bool succeeded = true)
    {
        SemanticType voidType = new("type:void", "void", "void", "primitive", true, false);
        SemanticType intType = new("type:int32", "int32", "int", "primitive", true, false);
        SemanticType boolType = new("type:bool", "bool", "bool", "primitive", true, false);
        SemanticType stringType = new("type:string", "string", "string", "class", false, false);
        SemanticType pointType = new(
            "type:global::Game.Point",
            "global::Game.Point",
            "Point",
            "struct",
            true,
            false);
        SemanticType enumType = new(
            "type:global::Game.Mode",
            "global::Game.Mode",
            "Mode",
            "enum",
            true,
            false);
        SemanticType arrayType = new("type:int32[]", "int32[]", "int[]", "array", false, false);
        SemanticType scriptType = new(
            "type:global::Game.Script",
            "global::Game.Script",
            "Script",
            "class",
            false,
            false);
        SemanticSpan span = Span();
        SemanticSymbol[] symbols =
        {
            new(StateFieldId, "field", "Score", "symbol:type:global::Game.Script", intType.Id,
                "int32 Score", true, "private", span),
            new("symbol:field:global::Game.Point.X:int32", "field", "X",
                "symbol:type:global::Game.Point", intType.Id, "int32 X", false, "public", span),
            new("symbol:field:global::Game.Point.Y:int32", "field", "Y",
                "symbol:type:global::Game.Point", intType.Id, "int32 Y", false, "public", span),
        };
        SemanticCallable main = new(
            MainMethodId,
            scriptType.Id,
            voidType.Id,
            Array.Empty<SemanticCallableParameter>(),
            true,
            false,
            true,
            null,
            null,
            new SemanticCallableExport("guest_main"));
        SemanticCallable host = new(
            HostMethodId,
            "type:global::Game.Host",
            intType.Id,
            new[]
            {
                new SemanticCallableParameter(0, "symbol:parameter:host:0", "left", intType.Id, "none"),
                new SemanticCallableParameter(1, "symbol:parameter:host:1", "right", intType.Id, "none"),
            },
            true,
            false,
            false,
            null,
            new SemanticCallableImport("env", "host_add"),
            null);
        SemanticControlFlowEdge entryEdge = new(0, 1, "fallthrough", "regular");
        SemanticControlFlowEdge returnEdge = new(1, 2, "fallthrough", "return");
        SemanticControlFlowGraph graph = new(
            MainMethodId,
            0,
            2,
            new[]
            {
                new SemanticBasicBlock(0, "entry", true, "none", Array.Empty<SemanticOperation>(), null,
                    Array.Empty<SemanticControlFlowEdge>(), new[] { entryEdge }),
                new SemanticBasicBlock(1, "block", true, "none", Array.Empty<SemanticOperation>(), null,
                    new[] { entryEdge }, new[] { returnEdge }),
                new SemanticBasicBlock(2, "exit", true, "none", Array.Empty<SemanticOperation>(), null,
                    new[] { returnEdge }, Array.Empty<SemanticControlFlowEdge>()),
            });
        SemanticDiagnostic[] diagnostics = succeeded
            ? Array.Empty<SemanticDiagnostic>()
            : new[] { new SemanticDiagnostic("ASCS_TEST", "error", "synthetic failure", span) };
        SemanticDocument document = new SemanticDocument(
            4,
            "csharp",
            "1.4",
            new SemanticSource("Scripts/Test.cs", new string('a', 64), new string('b', 64), 0),
            succeeded,
            new[] { voidType, intType, boolType, stringType, pointType, enumType, arrayType, scriptType },
            new[]
            {
                new SemanticTypeShape(enumType.Id, null, intType.Id),
                new SemanticTypeShape(arrayType.Id, intType.Id, null),
            },
            symbols,
            new[] { main, host },
            new[] { new SemanticMethodBody(MainMethodId, Operation("method_body", null)) },
            new[] { graph },
            null,
            diagnostics);
        return document with
        {
            StateContracts = new[]
            {
                new SemanticStateContract(
                    scriptType.Id,
                    "compatible",
                    1,
                    new[]
                    {
                        new SemanticStateFieldContract(
                            StateFieldId,
                            "implicit",
                            Array.Empty<string>()),
                    }),
            },
        };
    }

    public static SemanticOperation Operation(
        string kind,
        string? typeId,
        string? symbolId = null,
        IReadOnlyList<SemanticOperation>? children = null,
        SemanticConstant? constant = null,
        string? operatorKind = null,
        string? captureId = null,
        SemanticConversion? conversion = null)
    {
        return new SemanticOperation(
            kind,
            true,
            operatorKind,
            false,
            false,
            false,
            false,
            typeId,
            symbolId,
            Array.Empty<string>(),
            constant,
            conversion,
            null,
            null,
            captureId,
            Span(),
            children ?? Array.Empty<SemanticOperation>());
    }

    private static SemanticSpan Span()
    {
        return new SemanticSpan(0, 0, 0, 0, 0, 0);
    }
}
