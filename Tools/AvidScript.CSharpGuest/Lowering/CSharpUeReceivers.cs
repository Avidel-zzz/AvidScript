using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

// A UE receiver is a weak host identity, never a managed UObject allocation.
internal static class CSharpUeReceivers
{
    public static bool IsType(SemanticDocument document, string type) => document.SchemaVersion >= 25
        && document.UeTypeDeclarations.Any(item => item.TypeId == type);
    public static bool IsView(SemanticDocument document, string type) => IsType(document, type) || CSharpUeDispatch.IsInterfaceView(document, type);
    public static string Import(string type) => "import:$ue:receiver:" + type;

    public static bool TryEquality(CSharpFunctionLoweringContext context, SemanticOperation operation, int block,
        List<GuestInstruction> instructions, out GuestRegister? result)
    {
        result = null;
        if (operation.OperatorKind is not ("equals" or "not_equals") || operation.SymbolId is not null
            || operation.IsLifted || operation.TypeId != "type:bool" || operation.Children.Count != 2) return false;
        SemanticOperation[] operands = operation.Children.Select(Unwrap).ToArray();
        if (!operands.Any(item => item.TypeId is { } type && IsView(context.Document, type))
            || operands.Any(item => item.Constant?.Kind != "null" && (item.TypeId is null || !IsView(context.Document, item.TypeId)))) return false;
        List<string> values = new();
        foreach (SemanticOperation operand in operands)
        {
            GuestRegister normalized = context.CreateTemporary("type:uint64", block)!;
            if (operand.Constant?.Kind == "null")
                instructions.Add(new("constant", normalized.Id, Array.Empty<string>(), null, null, new("zero", null)));
            else
            {
                GuestRegister? value = CSharpOperationLowerer.LowerValue(context, operand, block, instructions);
                if (value is null) return true;
                instructions.Add(new("convert", normalized.Id, new[] { value.Id }, null, null, null));
            }
            values.Add(normalized.Id);
        }
        result = context.CreateTemporary("type:bool", block)!;
        instructions.Add(new("binary", result.Id, values, null, operation.OperatorKind, null));
        return true;

        static SemanticOperation Unwrap(SemanticOperation item)
        {
            while (item is { Kind: "conversion", TypeId: "type:object", Children.Count: 1,
                Conversion: { IsImplicit: true, IsReference: true, IsUserDefined: false } }) item = item.Children[0];
            return item;
        }
    }

    public static void Require(CSharpFunctionLoweringContext context, GuestRegister receiver, int block, List<GuestInstruction> instructions)
    {
        GuestRegister valid = context.CreateTemporary("type:int32", block)!;
        instructions.Add(new("call", valid.Id, new[] { receiver.Id }, Import(receiver.TypeId), null, null));
    }

    public static void AddGuards(SemanticDocument document, List<GuestFunction> functions)
    {
        var methods = document.Callables.ToDictionary(item => CSharpGuestIds.Function(item.MethodSymbolId), StringComparer.Ordinal);
        for (int i = 0; i < functions.Count; ++i)
        {
            GuestFunction function = functions[i];
            if (!methods.TryGetValue(function.Id, out SemanticCallable? callable)) continue;
            List<GuestInstruction> guards = new();
            List<GuestRegister> locals = function.Locals.ToList();
            if (!callable.IsStatic && IsType(document, callable.ContainingTypeId)) Guard(function.Parameters[0]);
            foreach (SemanticCallableParameter parameter in callable.Parameters)
            {
                if (CSharpClosureLayout.CapturedParameter(document, parameter.SymbolId) is not { } captured
                    || captured.Cell.Kind != "receiver" || !IsType(document, captured.Cell.TypeId)) continue;
                GuestRegister environment = function.Parameters.Single(item => item.Id == CSharpGuestIds.Parameter(parameter.SymbolId));
                GuestRegister receiver = new("$ue:captured:" + locals.Count, captured.Cell.TypeId);
                locals.Add(receiver);
                guards.Add(new("managed_get", receiver.Id, new[] { environment.Id }, captured.Cell.SymbolId, null, null));
                Guard(receiver);
            }
            if (guards.Count != 0)
                functions[i] = function with { Locals = locals, Blocks = function.Blocks.Select(block => block.Id == function.EntryBlockId
                    ? block with { Instructions = guards.Concat(block.Instructions).ToArray() } : block).ToArray() };

            void Guard(GuestRegister receiver)
            {
                GuestRegister valid = new("$ue:valid:" + locals.Count, "type:int32");
                locals.Add(valid);
                guards.Add(new("call", valid.Id, new[] { receiver.Id }, Import(receiver.TypeId), null, null));
            }
        }
    }

    public static GuestImport[] AppendImports(SemanticDocument document, IReadOnlyList<GuestImport> imports, IReadOnlyList<GuestFunction> functions)
    {
        var calls = functions.SelectMany(function => function.Blocks).SelectMany(block => block.Instructions)
            .Where(instruction => instruction.Op == "call").Select(instruction => instruction.TargetId).ToHashSet(StringComparer.Ordinal);
        return imports.Concat(document.UeTypeDeclarations.Select((type, ordinal) => new { type, ordinal })
            .Where(item => calls.Contains(Import(item.type.TypeId)))
            .Select(item => new GuestImport(Import(item.type.TypeId), SemanticUeTypeRuntimeContract.HostModule,
                "avid_ue_receiver_" + item.ordinal.ToString(CultureInfo.InvariantCulture) + "_require_v1",
                new[] { item.type.TypeId }, "type:int32"))).ToArray();
    }
}
