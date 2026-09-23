using System.Runtime.InteropServices;

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
}
