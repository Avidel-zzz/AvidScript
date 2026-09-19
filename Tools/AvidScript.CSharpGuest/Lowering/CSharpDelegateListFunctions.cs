using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using B = AvidScript.CSharpGuest.CSharpDelegateListBuilder;
using L = AvidScript.CSharpGuest.CSharpDelegateComposition;

namespace AvidScript.CSharpGuest;

internal static class CSharpDelegateListFunctions
{
    public static IReadOnlyList<GuestFunction> Build(SemanticDocument document, SemanticDelegateType signature)
        => new[] { Count(signature.TypeId), Item(signature.TypeId), Combine(signature.TypeId), Invoke(document, signature),
            Equal(signature.TypeId), CSharpDelegateListRemoval.Build(signature.TypeId) };

    private static GuestFunction Count(string type)
    {
        B b = new(type, "count", B.Int, new GuestRegister("d", type));
        string target = b.Target("d");
        b.Branch(b.Binary("equals", target, b.Zero(CSharpClosureLayout.FunctionType(type), "null")), "null", "kind");
        b.At("null"); b.Return(b.Integer(0));
        b.At("kind"); b.Branch(b.Binary("equals", target, b.Invoker()), "node", "single");
        b.At("single"); b.Return(b.Integer(1));
        b.At("node"); b.Return(b.Field(b.Node(b.Context("d")), L.Count, B.Int, true));
        return b.Finish();
    }

    private static GuestFunction Item(string type)
    {
        B b = new(type, "item", type, new("d", type), new("index", B.Int));
        string current = b.Local(type), index = b.Local(B.Int);
        b.Copy(current, "d"); b.Copy(index, "index");
        string invoker = b.Invoker();
        b.Branch(b.Binary("less_than", index, b.Integer(0)), "invalid", "upper");
        b.At("upper"); b.Branch(b.Binary("less_than", index, b.Call("count", B.Int, current)), "walk", "invalid");
        b.At("walk"); b.Branch(b.Binary("equals", b.Target(current), invoker), "node", "leaf");
        b.At("node"); string node = b.Node(b.Context(current)); string leftCount = b.Field(node, L.LeftCount, B.Int, true);
        b.Branch(b.Binary("less_than", index, leftCount), "left", "right");
        b.At("left"); b.Copy(current, b.Field(node, L.Left, type, true)); b.Jump("walk");
        b.At("right"); b.Copy(current, b.Field(node, L.Right, type, true));
        b.Copy(index, b.Binary("subtract", index, leftCount, B.Int)); b.Jump("walk");
        b.At("leaf"); b.Return(current);
        b.At("invalid"); b.Trap();
        return b.Finish();
    }

    private static GuestFunction Combine(string type)
    {
        B b = new(type, "add", type, new("a", type), new("b", type));
        string zero = b.Integer(0), na = b.Call("count", B.Int, "a");
        b.Branch(b.Binary("equals", na, zero), "right", "left-count");
        b.At("right"); b.Return("b");
        b.At("left-count"); string nb = b.Call("count", B.Int, "b");
        b.Branch(b.Binary("equals", nb, zero), "left", "limit");
        b.At("left"); b.Return("a");
        b.At("limit"); string limit = b.Binary("subtract", b.Integer(int.MaxValue), na, B.Int);
        b.Branch(b.Binary("less_than", limit, nb), "overflow", "allocate");
        b.At("overflow"); b.Trap();
        b.At("allocate");
        string node = b.Value(CSharpClosureLayout.Reference(L.Node(type)), "managed_new", Array.Empty<string>());
        b.Set(node, L.Left, "a", true); b.Set(node, L.Right, "b", true);
        b.Set(node, L.LeftCount, na, true); b.Set(node, L.Count, b.Binary("add", na, nb, B.Int), true);
        string result = b.Zero(type);
        b.Set(result, CSharpClosureLayout.TargetField, b.Invoker());
        b.Set(result, CSharpClosureLayout.ContextField, b.Value(CSharpClosureLayout.ObjectType, "managed_cast", new[] { node }));
        b.Return(result);
        return b.Finish();
    }

    private static GuestFunction Invoke(SemanticDocument document, SemanticDelegateType signature)
    {
        string type = signature.TypeId;
        GuestRegister[] parameters = new[] { new GuestRegister("context", CSharpClosureLayout.ObjectType) }
            .Concat(signature.Parameters.Select(parameter => new GuestRegister("argument:" + parameter.Ordinal,
                CSharpBorrowedReferences.Parameter(document, parameter.TypeId, parameter.RefKind)))).ToArray();
        B b = new(type, "invoke", signature.ReturnTypeId, parameters);
        string list = b.Zero(type);
        b.Set(list, CSharpClosureLayout.TargetField, b.Invoker()); b.Set(list, CSharpClosureLayout.ContextField, "context");
        string count = b.Call("count", B.Int, list), index = b.Integer(0), one = b.Integer(1);
        string? returned = signature.ReturnTypeId == B.Void ? null : b.Local(signature.ReturnTypeId);
        b.Jump("test");
        b.At("test"); b.Branch(b.Binary("less_than", index, count), "call", "done");
        b.At("call"); string leaf = b.Call("item", type, list, index);
        string target = b.Target(leaf), context = b.Context(leaf);
        b.Emit("call_indirect", returned, new[] { target, context }.Concat(parameters.Skip(1).Select(parameter => parameter.Id)).ToArray(),
            CSharpClosureLayout.FunctionType(type));
        b.Copy(index, b.Binary("add", index, one, B.Int)); b.Jump("test");
        b.At("done"); b.Return(returned);
        return b.Finish();
    }

    private static GuestFunction Equal(string type)
    {
        B b = new(type, "equals", B.Bool, new("a", type), new("b", type));
        string count = b.Call("count", B.Int, "a");
        string index = b.Integer(0), one = b.Integer(1);
        b.Branch(b.Binary("equals", count, b.Call("count", B.Int, "b")), "test", "false");
        b.At("test"); b.Branch(b.Binary("less_than", index, count), "compare", "true");
        b.At("compare"); string a = b.Call("item", type, "a", index), c = b.Call("item", type, "b", index);
        string same = b.Value(B.Bool, "call", new[] { a, c }, CSharpDelegateIdentityLowerer.Function(type));
        b.Branch(same, "next", "false");
        b.At("next"); b.Copy(index, b.Binary("add", index, one, B.Int)); b.Jump("test");
        b.At("true"); b.Return(b.Value(B.Bool, "constant", Array.Empty<string>(), constant: new("bool", "1")));
        b.At("false"); b.Return(b.Value(B.Bool, "constant", Array.Empty<string>(), constant: new("bool", "0")));
        return b.Finish();
    }
}
