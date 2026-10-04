using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using AvidScript.GuestIr;

internal static class GuestTaskValuePlanTests
{
    public static int Run()
    {
        ScalarAndEnumPlans(); NestedReferencesAndPadding(); ReachableShapeIdentity();
        ResourcesStayDistinct(); BorrowedAliasesAreRejected(); CanonicalLayoutIsRequired();
        MalformedAndOversizedTypes(); DepthAndLeafLimits(); PacketTampering(); NativeAbiParity();
        ExportNativeFixtures();
        return 10;
    }

    private static void Check(bool condition, string message)
    { if (!condition) throw new InvalidOperationException(message); }
    private static void Reject(Action action, string message)
    {
        try { action(); } catch (ArgumentException) { return; }
        throw new InvalidOperationException(message);
    }
    private static GuestType Scalar(string id, string storage, int size) =>
        new(id, "scalar", storage, Array.Empty<GuestField>(), null, null, size, size);
    private static GuestType Struct(string id, params (string Name, string Type)[] fields) =>
        new(id, "struct", "memory", fields.Select(field => new GuestField(
            id + ":" + field.Name, field.Name, field.Type, 0)).ToArray(), null, null, 0, 1);
    private static GuestType[] Layout(params GuestType[] types)
    {
        GuestTypeLayoutResult result = GuestDataLayout.ComputeTypes(types);
        Check(result.Succeeded, "fixture layouts must be valid"); return result.Types.ToArray();
    }
    private static GuestType[] NestedTypes() => Layout(
        Scalar("s:u8", "i32", 1), Scalar("s:i32", "i32", 4), Scalar("s:f32", "f32", 4),
        new("s:mode", "enum", "i32", Array.Empty<GuestField>(), null, "s:i32", 0, 1),
        new("r:玩家", "managed_ref", "i64", Array.Empty<GuestField>(), "p:node", null, 8, 8),
        Struct("p:node", ("Health", "s:i32"), ("Next", "r:玩家")),
        Struct("v:inner", ("Damage", "s:f32"), ("Player", "r:玩家")),
        Struct("v:result", ("Tag", "s:u8"), ("Inner", "v:inner"), ("Alias", "r:玩家"), ("Mode", "s:mode")));

    private static void ScalarAndEnumPlans()
    {
        foreach (var shape in new[] { ("i32", 1), ("i32", 2), ("i32", 4), ("i64", 8), ("f32", 4), ("f64", 8) })
        {
            GuestTaskValuePlan plan = GuestTaskValuePlan.Build(new[] { Scalar("s:value", shape.Item1, shape.Item2) }, "s:value");
            Check(!plan.RequiresLease && plan.Size == shape.Item2 && plan.Leaves.Count == 1, "scalar result shape changed");
            Check(plan.Leaves[0].Kind == (shape.Item1[0] == 'f' ? GuestTaskValueLeafKind.Float : GuestTaskValueLeafKind.Integer), "float bits cannot become integer values");
        }
        GuestTaskValuePlan empty = GuestTaskValuePlan.Build(Layout(Struct("v:empty")), "v:empty");
        Check(empty.Size == 0 && empty.Leaves.Count == 0 && !empty.RequiresLease, "empty structs remain valid values");
        GuestTaskValuePlan mode = GuestTaskValuePlan.Build(NestedTypes(), "s:mode");
        Check(mode.TypeId == "s:mode" && mode.Size == 4 && mode.Leaves[0].Kind == GuestTaskValueLeafKind.Integer,
            "enum nominal identity must survive integer storage");
    }
    private static void NestedReferencesAndPadding()
    {
        GuestTaskValuePlan plan = GuestTaskValuePlan.Build(NestedTypes(), "v:result");
        Check(plan.Size == 40 && plan.Alignment == 8 && plan.RequiresLease, "nested result shape or ownership is wrong");
        Check(plan.Leaves.Select(leaf => leaf.Offset).SequenceEqual(new[] { 0, 8, 16, 24, 32 }), "nested field offsets changed");
        Check(plan.Leaves.Where(leaf => leaf.Kind == GuestTaskValueLeafKind.ManagedReference)
            .All(leaf => leaf.TypeId == "r:玩家" && leaf.TargetTypeId == "p:node"), "references need nominal target identities");
        Check(GuestTaskValuePlan.Matches(NestedTypes(), "v:result", plan.Encode()), "canonical plan should admit");
    }
    private static void ReachableShapeIdentity()
    {
        GuestType[] types = NestedTypes();
        GuestTaskValuePlan original = GuestTaskValuePlan.Build(types, "v:result");
        Check(original.Encode().SequenceEqual(GuestTaskValuePlan.Build(types.Reverse().ToArray(), "v:result").Encode()), "table order must not change a result plan");
        GuestType[] extra = types.Append(Scalar("unused", "i64", 8)).ToArray();
        Check(original.ShapeSha256 == GuestTaskValuePlan.Build(extra, "v:result").ShapeSha256, "unreachable types must not perturb identity");
        GuestType payload = types.Single(type => type.Id == "p:node");
        GuestType changed = payload with { Fields = payload.Fields.Select(field => field.Name == "Health"
            ? field with { Name = "Life", Id = "p:node:Life" } : field).ToArray() };
        GuestType[] updated = types.Select(type => type.Id == payload.Id ? changed : type).ToArray();
        Check(original.ShapeSha256 != GuestTaskValuePlan.Build(updated, "v:result").ShapeSha256,
            "managed payload changes must change the outer result identity");
        Check(!GuestTaskValuePlan.Matches(updated, "v:result", original.Encode()), "stale payload shapes must reject");
    }
    private static void ResourcesStayDistinct()
    {
        foreach (var entry in ResourceTypes())
        {
            GuestTaskValuePlan plan = GuestTaskValuePlan.Build(Layout(Scalar("s:element", "i32", 4), entry.Type), entry.Type.Id);
            Check(plan.RequiresLease && plan.Leaves.Single().Kind == entry.Kind, "resources cannot silently fall back to copied bits");
        }
    }
    private static IEnumerable<(GuestType Type, GuestTaskValueLeafKind Kind)> ResourceTypes()
    {
        foreach (var pair in new[] { ("string", GuestTaskValueLeafKind.LinearString), ("array", GuestTaskValueLeafKind.LinearArray),
            ("handle", GuestTaskValueLeafKind.UeHandle), ("class_ref", GuestTaskValueLeafKind.ClassReference),
            ("factory_ref", GuestTaskValueLeafKind.FactoryReference), ("object_type_ref", GuestTaskValueLeafKind.ObjectTypeReference),
            ("composite_ref", GuestTaskValueLeafKind.CompositeReference), ("function_ref", GuestTaskValueLeafKind.FunctionReference) })
        {
            int size = pair.Item1 == "handle" ? 8 : 4;
            yield return (new("r:" + pair.Item1, pair.Item1, size == 8 ? "i64" : "i32", Array.Empty<GuestField>(),
                pair.Item1 == "array" ? "s:element" : null, null, size, size), pair.Item2);
        }
    }
    private static void BorrowedAliasesAreRejected()
    {
        GuestType i32 = Scalar("s:i32", "i32", 4);
        GuestType owner = new("r:object", "managed_ref", "i64", Array.Empty<GuestField>(), null, null, 8, 8);
        GuestType borrow = GuestBorrowedReference.Declare("b:value", i32.Id, owner.Id, i32.Id);
        GuestType reference = new("r:bad", "managed_ref", "i64", Array.Empty<GuestField>(), "p:bad", null, 8, 8);
        GuestType[] types = Layout(i32, owner, borrow, Struct("p:bad", ("Borrow", borrow.Id)), reference);
        Reject(() => GuestTaskValuePlan.Build(types, borrow.Id), "borrowed result should reject");
        Reject(() => GuestTaskValuePlan.Build(types, reference.Id), "borrow hidden in an object payload should reject");
    }
    private static void CanonicalLayoutIsRequired()
    {
        GuestType[] types = NestedTypes();
        GuestType root = types.Single(type => type.Id == "v:result");
        foreach (GuestType bad in new[] { root with { Size = 48 }, root with { Alignment = 4 },
            root with { Fields = root.Fields.Select((field, index) => index == 1 ? field with { Offset = 4 } : field).ToArray() } })
            Reject(() => GuestTaskValuePlan.Build(types.Select(type => type.Id == root.Id ? bad : type).ToArray(), root.Id),
                "caller-supplied offsets cannot become canonical by recomputation");
    }
    private static void MalformedAndOversizedTypes()
    {
        GuestType scalar = Scalar("s:i32", "i32", 4);
        Reject(() => GuestTaskValuePlan.Build(new[] { scalar, scalar }, scalar.Id), "duplicate types must reject");
        Reject(() => GuestTaskValuePlan.Build(new[] { scalar with { Id = "bad\0id" } }, "bad\0id"), "NUL identity should reject");
        Reject(() => GuestTaskValuePlan.Build(new[] { scalar with { Id = "\ud800" } }, "\ud800"), "invalid UTF-8 identity should reject");
        Reject(() => GuestTaskValuePlan.Build(new[] { scalar with { Id = new string('x', 1025) } }, "missing"), "oversized identity should reject");
        GuestType duplicate = Struct("v:duplicate", ("Field", scalar.Id), ("Field", scalar.Id));
        Reject(() => GuestTaskValuePlan.Build(Layout(scalar, duplicate), duplicate.Id), "duplicate fields must reject");
        Reject(() => GuestTaskValuePlan.Build(new[] { scalar }, "missing"), "unknown result type should reject");
        Reject(() => GuestTaskValuePlan.Build(Layout(new GuestType("v:void", "void", "none", Array.Empty<GuestField>(), null, null, 0, 1)), "v:void"), "void is not Task<T>");
    }
    private static void DepthAndLeafLimits()
    {
        List<GuestType> deep = new() { Scalar("s:i32", "i32", 4) };
        for (int i = 0; i < 130; ++i) deep.Add(Struct("v:" + i, ("Child", i == 0 ? "s:i32" : "v:" + (i - 1))));
        Reject(() => GuestTaskValuePlan.Build(deep, "v:129"), "deep type dependencies should fail before recursive layout");
        GuestType wide = Struct("v:wide", Enumerable.Range(0, 257).Select(i => ("Field" + i, "s:i32")).ToArray());
        Reject(() => GuestTaskValuePlan.Build(new[] { deep[0], wide }, wide.Id), "wide type should reject");
        GuestType[] large = Layout(Scalar("s:i64", "i64", 8), Struct("v:half", Enumerable.Range(0, 256).Select(i => ("Field" + i, "s:i64")).ToArray()),
            Struct("v:big", ("A", "v:half"), ("B", "v:half"), ("C", "v:half")));
        Reject(() => GuestTaskValuePlan.Build(large, "v:big"), "oversized nested result should reject");
    }
    private static void PacketTampering()
    {
        GuestType[] types = NestedTypes(); byte[] packet = GuestTaskValuePlan.Build(types, "v:result").Encode();
        for (int i = 0; i < packet.Length; ++i)
        {
            byte[] mutated = packet.ToArray(); mutated[i] ^= 1;
            Check(!GuestTaskValuePlan.Matches(types, "v:result", mutated), "every published plan byte must match its canonical source");
        }
        Check(!GuestTaskValuePlan.Matches(types, "v:result", packet.AsSpan(0, packet.Length - 1)), "truncated plan should reject");
        Check(!GuestTaskValuePlan.Matches(types, "v:result", packet.Append((byte)0).ToArray()), "trailing data should reject");
        Check(!GuestTaskValuePlan.Matches(types, "v:inner", packet), "same module does not grant another result type");
    }
    private static void NativeAbiParity()
    {
        string headerPath = Path.Combine(Directory.GetCurrentDirectory(), "Source", "AvidScriptCore", "Public", "AvidScriptTaskValuePlanAbi.h");
        string header = File.ReadAllText(headerPath);
        foreach (var constant in new[] { ("Magic", (long)GuestTaskValuePlan.Magic), ("Version", (long)GuestTaskValuePlan.Version),
            ("MaxValueBytes", (long)GuestTaskValuePlan.MaxValueBytes), ("MaxLeaves", (long)GuestTaskValuePlan.MaxLeaves),
            ("MaxIdentityBytes", (long)GuestTaskValuePlan.MaxIdentityBytes), ("MaxPlanBytes", (long)GuestTaskValuePlan.MaxPlanBytes),
            ("ShapeHashBytes", (long)GuestTaskValuePlan.ShapeHashBytes) })
        {
            string expression = Regex.Match(header, @"\b" + constant.Item1 + @"\s*=\s*([^;]+);").Groups[1].Value.Trim();
            long value = expression.StartsWith("0x", StringComparison.Ordinal) ? Convert.ToInt64(expression[2..], 16)
                : expression.Split('*').Select(part => long.Parse(part.Trim())).Aggregate(1L, (a, b) => a * b);
            Check(value == constant.Item2, "Native plan constant drifted: " + constant.Item1);
        }
        foreach (GuestTaskValueLeafKind kind in Enum.GetValues<GuestTaskValueLeafKind>())
            Check(Regex.IsMatch(header, @"\b" + kind + @"\s*=\s*" + (int)kind + @"\b"), "Native leaf kind drifted: " + kind);
    }
    private static void ExportNativeFixtures()
    {
        string? directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_TASK_VALUE_PLAN_DIR");
        if (string.IsNullOrEmpty(directory)) return;
        Directory.CreateDirectory(directory);
        foreach (var shape in new[] { ("i32", 1), ("i32", 2), ("i32", 4), ("i64", 8), ("f32", 4), ("f64", 8) })
            File.WriteAllBytes(Path.Combine(directory, $"scalar_{shape.Item1}_{shape.Item2}.plan"),
                GuestTaskValuePlan.Build(new[] { Scalar("s:value", shape.Item1, shape.Item2) }, "s:value").Encode());
        File.WriteAllBytes(Path.Combine(directory, "nested.plan"), GuestTaskValuePlan.Build(NestedTypes(), "v:result").Encode());
        File.WriteAllBytes(Path.Combine(directory, "empty.plan"), GuestTaskValuePlan.Build(Layout(Struct("v:empty")), "v:empty").Encode());
        foreach (var entry in ResourceTypes())
            File.WriteAllBytes(Path.Combine(directory, entry.Type.Kind + ".plan"),
                GuestTaskValuePlan.Build(Layout(Scalar("s:element", "i32", 4), entry.Type), entry.Type.Id).Encode());
        Console.WriteLine("AvidScript.TaskValuePlan.Fixtures: 16/16 emitted");
    }
}
