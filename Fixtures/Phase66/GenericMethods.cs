using System.Runtime.InteropServices;

public struct Pair<T, U>
{
    public T First;
    public U Second;
}

public sealed class Box<T>
{
    public T Value;
}

public sealed class MemberBox<T>
{
    public T Value;

    public MemberBox(T value) { Value = value; }
    public T Read() => Value;
    public T Relay() => Read();
    public void Store(T value) { Value = value; }
}

public static class Script
{
    static T Identity<T>(T value) => value;
    static T Forward<T>(T value) => Identity<T>(value);
    static T Bounce<T>(int count, T value) =>
        count == 0 ? value : Bounce<T>(count - 1, value);
    static U Second<T, U>(T first, U second) => second;
    static void Swap<T>(ref T left, ref T right)
    {
        T temporary = left;
        left = right;
        right = temporary;
    }
    static void Set<T>(out T value, T input) => value = input;
    static T[] EchoArray<T>(T[] values) => values;
    static Pair<T, U> MakePair<T, U>(T first, U second)
    {
        Pair<T, U> pair = default;
        pair.First = first;
        pair.Second = second;
        return pair;
    }
    static Box<T> MakeBox<T>(T value)
    {
        Box<T> box = new Box<T>();
        box.Value = value;
        return box;
    }
    static MemberBox<T> MakeMemberBox<T>(T value) => new MemberBox<T>(value);

    [UnmanagedCallersOnly(EntryPoint = "generic_int")]
    public static int Int() => Identity<int>(7);
    [UnmanagedCallersOnly(EntryPoint = "generic_float")]
    public static float Float() => Identity<float>(2.5f);
    [UnmanagedCallersOnly(EntryPoint = "generic_forward")]
    public static int Forwarded() => Forward<int>(3);
    [UnmanagedCallersOnly(EntryPoint = "generic_bounce")]
    public static int Bounced() => Bounce<int>(2, 4);
    [UnmanagedCallersOnly(EntryPoint = "generic_pair")]
    public static int Pair() => Second<float, int>(2.5f, 9)
        + (int)Second<int, float>(1, 3.5f);
    [UnmanagedCallersOnly(EntryPoint = "generic_ref_out")]
    public static int RefOut()
    {
        int left = 3;
        int right = 5;
        Swap<int>(ref left, ref right);
        Set<int>(out right, 7);
        return left * 10 + right;
    }
    [UnmanagedCallersOnly(EntryPoint = "generic_array")]
    public static int Array() => EchoArray<int>(new[] { 3, 5 })[1];
    [UnmanagedCallersOnly(EntryPoint = "generic_named_int_float")]
    public static int NamedIntFloat() => MakePair<int, float>(7, 2.5f).First;
    [UnmanagedCallersOnly(EntryPoint = "generic_named_float_int")]
    public static int NamedFloatInt() => MakePair<float, int>(2.5f, 9).Second;
    [UnmanagedCallersOnly(EntryPoint = "generic_named_nested")]
    public static int NamedNested() =>
        MakePair<Pair<int, float>, int>(MakePair<int, float>(7, 2.5f), 4).First.First;
    [UnmanagedCallersOnly(EntryPoint = "generic_box_int")]
    public static int BoxInt()
    {
        Box<int> first = MakeBox<int>(7);
        Box<int> second = MakeBox<int>(4);
        first.Value += 2;
        return first.Value * 10 + second.Value;
    }
    [UnmanagedCallersOnly(EntryPoint = "generic_box_nested")]
    public static int BoxNested()
    {
        Box<Pair<int, float>> box = MakeBox(MakePair<int, float>(7, 2.5f));
        box.Value.First += 1;
        return box.Value.First;
    }
    [UnmanagedCallersOnly(EntryPoint = "generic_member_int")]
    public static int MemberInt()
    {
        MemberBox<int> first = MakeMemberBox<int>(7);
        MemberBox<int> second = new MemberBox<int>(4);
        first.Store(first.Read() + 2);
        return first.Relay() * 10 + second.Read();
    }
    [UnmanagedCallersOnly(EntryPoint = "generic_member_nested")]
    public static int MemberNested()
    {
        MemberBox<Pair<int, float>> box = MakeMemberBox(
            MakePair<int, float>(7, 2.5f));
        Pair<int, float> value = box.Read();
        value.First += 1;
        box.Store(value);
        return box.Relay().First;
    }
}
