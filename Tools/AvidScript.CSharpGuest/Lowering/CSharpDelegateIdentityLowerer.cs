using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal static class CSharpDelegateIdentityLowerer
{
    private const string Bool = "type:bool";
    public static string Function(string type) => "function:$delegate:equals:" + type;

    public static GuestRegister? Lower(CSharpFunctionLoweringContext context, SemanticOperation operation, int block,
        List<GuestInstruction> instructions)
    {
        if (operation.OperatorKind is not ("equals" or "not_equals") || operation.TypeId != Bool || operation.Children.Count != 2
            || operation.Children[0].TypeId != operation.Children[1].TypeId)
        { context.Add("ASCG1024", "Delegate comparison requires matching nominal signatures and equality operators."); return null; }
        GuestRegister? left = CSharpOperationLowerer.LowerValue(context, operation.Children[0], block, instructions);
        GuestRegister? right = CSharpOperationLowerer.LowerValue(context, operation.Children[1], block, instructions);
        GuestRegister? result = context.CreateTemporary(Bool, block);
        if (left is null || right is null || result is null) return null;
        if (!CSharpClosureLayout.UsesManagedDelegates(context.Document))
            instructions.Add(new("binary", result.Id, new[] { left.Id, right.Id }, null, operation.OperatorKind, null));
        else
        {
            string comparison = CSharpDelegateComposition.Signatures(context.Document).Contains(left.TypeId)
                ? CSharpDelegateComposition.Function(left.TypeId, "equals") : Function(left.TypeId);
            instructions.Add(new("call", result.Id, new[] { left.Id, right.Id }, comparison, null, null));
            if (operation.OperatorKind == "not_equals")
            {
                GuestRegister zero = context.CreateTemporary(Bool, block)!;
                GuestRegister inverted = context.CreateTemporary(Bool, block)!;
                instructions.Add(new("constant", zero.Id, Array.Empty<string>(), null, null, new("bool", "0")));
                instructions.Add(new("binary", inverted.Id, new[] { result.Id, zero.Id }, null, "equals", null));
                return inverted;
            }
        }
        return result;
    }

    public static IReadOnlyList<GuestFunction> Build(SemanticDocument document, IReadOnlyList<GuestFunction> functions)
    {
        if (!CSharpClosureLayout.UsesManagedDelegates(document)) return Array.Empty<GuestFunction>();
        HashSet<string> calls = functions.SelectMany(function => function.Blocks).SelectMany(block => block.Instructions)
            .Where(instruction => instruction.Op == "call").Select(instruction => instruction.TargetId!).ToHashSet(StringComparer.Ordinal);
        HashSet<string> existing = functions.Select(function => function.Id).ToHashSet(StringComparer.Ordinal);
        return document.DelegateTypes.Where(signature => calls.Contains(Function(signature.TypeId)))
            .OrderBy(signature => signature.TypeId, StringComparer.Ordinal).Select(BuildOne).ToArray();

        GuestFunction BuildOne(SemanticDelegateType signature)
        {
            string functionType = CSharpClosureLayout.FunctionType(signature.TypeId);
            List<GuestRegister> locals = new(); List<GuestBasicBlock> blocks = new();
            string Local(string id, string type) { locals.Add(new(id, type)); return id; }
            string a = Local("target:a", functionType), b = Local("target:b", functionType), same = Local("same:target", Bool);
            blocks.Add(Block("entry", new[] { Field(a, "a", CSharpClosureLayout.TargetField), Field(b, "b", CSharpClosureLayout.TargetField), Eq(same, a, b) }, Branch(same, "context", "false")));
            string ca = Local("context:a", CSharpClosureLayout.ObjectType), cb = Local("context:b", CSharpClosureLayout.ObjectType), sameContext = Local("same:context", Bool);
            var targets = document.Callables.Select(callable => (Callable: callable, Thunk: CSharpClosureLayout.Thunk(callable.MethodSymbolId, signature.TypeId)))
                .Where(item => existing.Contains(item.Thunk) && CSharpClosureLayout.Environments(document, item.Callable.MethodSymbolId).Count > 0)
                .OrderBy(item => item.Thunk, StringComparer.Ordinal).ToArray();
            blocks.Add(Block("context", new[] { Field(ca, "a", CSharpClosureLayout.ContextField), Field(cb, "b", CSharpClosureLayout.ContextField), Eq(sameContext, ca, cb) },
                Branch(sameContext, "true", targets.Length == 0 ? "false" : "case:0")));
            for (int i = 0; i < targets.Length; i++)
            {
                var target = targets[i]; string prefix = "case:" + i;
                string candidate = Local(prefix + ":target", functionType), matches = Local(prefix + ":matches", Bool);
                blocks.Add(Block(prefix, new[] { new GuestInstruction("function_ref", candidate, Array.Empty<string>(), target.Thunk, null, null), Eq(matches, a, candidate) },
                    Branch(matches, prefix + ":cast", i + 1 < targets.Length ? "case:" + (i + 1) : "false")));
                string bindingType = CSharpClosureLayout.Reference(CSharpClosureLayout.Binding(target.Callable.MethodSymbolId));
                string ba = Local(prefix + ":a", bindingType), bb = Local(prefix + ":b", bindingType);
                blocks.Add(Block(prefix + ":cast", new[] { Cast(ba, ca), Cast(bb, cb) }, new("branch", null, prefix + ":env:0", null, null)));
                var environments = CSharpClosureLayout.Environments(document, target.Callable.MethodSymbolId);
                for (int j = 0; j < environments.Count; j++)
                {
                    var environment = environments[j]; string envPrefix = prefix + ":env:" + j;
                    string ea = Local(envPrefix + ":a", CSharpClosureLayout.Reference(environment.Id));
                    string eb = Local(envPrefix + ":b", CSharpClosureLayout.Reference(environment.Id));
                    string equal = Local(envPrefix + ":same", Bool);
                    List<GuestInstruction> compare = new() { Get(ea, ba, environment.Id), Get(eb, bb, environment.Id) };
                    // A receiver-only environment represents the original object identity.
                    // Environments with mutable captured locals retain activation identity.
                    if (environment.Cells.Count == 1 && environment.Cells[0].Kind == "receiver")
                    {
                        SemanticClosureCell receiver = environment.Cells[0];
                        string ra = Local(envPrefix + ":receiver:a", receiver.TypeId), rb = Local(envPrefix + ":receiver:b", receiver.TypeId);
                        compare.Add(Get(ra, ea, receiver.SymbolId)); compare.Add(Get(rb, eb, receiver.SymbolId));
                        compare.Add(Eq(equal, ra, rb));
                    }
                    else compare.Add(Eq(equal, ea, eb));
                    blocks.Add(Block(envPrefix, compare.ToArray(),
                        Branch(equal, j + 1 < environments.Count ? prefix + ":env:" + (j + 1) : "true", "false")));
                }
            }
            foreach (string truth in new[] { "true", "false" })
            {
                string value = Local(truth + ":value", Bool);
                blocks.Add(Block(truth, new[] { new GuestInstruction("constant", value, Array.Empty<string>(), null, null, new("bool", truth == "true" ? "1" : "0")) },
                    new("return", null, null, null, value)));
            }
            return new(Function(signature.TypeId), new[] { new GuestRegister("a", signature.TypeId), new GuestRegister("b", signature.TypeId) }, locals, Bool, "entry", blocks);
        }
    }
    private static GuestInstruction Field(string result, string owner, string field) => new("field_load", result, new[] { owner }, field, null, null);
    private static GuestInstruction Get(string result, string owner, string field) => new("managed_get", result, new[] { owner }, field, null, null);
    private static GuestInstruction Cast(string result, string owner) => new("managed_cast", result, new[] { owner }, null, null, null);
    private static GuestInstruction Eq(string result, string left, string right) => new("binary", result, new[] { left, right }, null, "equals", null);
    private static GuestTerminator Branch(string condition, string yes, string no) => new("branch_if", condition, yes, no, null);
    private static GuestBasicBlock Block(string id, GuestInstruction[] instructions, GuestTerminator terminator) => new(id, instructions, terminator);
}
