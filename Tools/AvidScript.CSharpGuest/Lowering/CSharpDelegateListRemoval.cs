using AvidScript.GuestIr;
using B = AvidScript.CSharpGuest.CSharpDelegateListBuilder;

namespace AvidScript.CSharpGuest;

internal static class CSharpDelegateListRemoval
{
    public static GuestFunction Build(string type)
    {
        B b = new(type, "subtract", type, new("a", type), new("b", type));
        string zero = b.Integer(0), one = b.Integer(1);
        string count = b.Call("count", B.Int, "a"), removed = b.Call("count", B.Int, "b");
        b.Branch(b.Binary("equals", removed, zero), "unchanged", "size");
        b.At("size"); b.Branch(b.Binary("less_than", count, removed), "unchanged", "search");
        b.At("search"); string start = b.Binary("subtract", count, removed, B.Int), offset = b.Local(B.Int);
        b.Jump("candidate");
        b.At("candidate"); b.Branch(b.Binary("less_than", start, zero), "unchanged", "reset");
        b.At("reset"); b.Copy(offset, zero); b.Jump("compare-test");
        b.At("compare-test"); b.Branch(b.Binary("less_than", offset, removed), "compare", "found");
        b.At("compare");
        string a = b.Call("item", type, "a", b.Binary("add", start, offset, B.Int));
        string c = b.Call("item", type, "b", offset);
        string equal = b.Value(B.Bool, "call", new[] { a, c }, CSharpDelegateIdentityLowerer.Function(type));
        b.Branch(equal, "advance", "previous");
        b.At("advance"); b.Copy(offset, b.Binary("add", offset, one, B.Int)); b.Jump("compare-test");
        b.At("previous"); b.Copy(start, b.Binary("subtract", start, one, B.Int)); b.Jump("candidate");
        b.At("found"); string result = b.Zero(type), index = b.Integer(0), end = b.Binary("add", start, removed, B.Int);
        b.Jump("copy-test");
        b.At("copy-test"); b.Branch(b.Binary("less_than", index, count), "skip-test", "done");
        b.At("skip-test"); b.Branch(b.Binary("equals", index, start), "skip", "copy");
        b.At("skip"); b.Copy(index, end); b.Jump("copy-test");
        b.At("copy"); b.Copy(result, b.Call("add", type, result, b.Call("item", type, "a", index)));
        b.Copy(index, b.Binary("add", index, one, B.Int)); b.Jump("copy-test");
        b.At("done"); b.Return(result);
        b.At("unchanged"); b.Return("a");
        return b.Finish();
    }
}
