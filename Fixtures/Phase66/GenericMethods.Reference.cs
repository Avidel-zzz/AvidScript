using System;

internal static unsafe class Program
{
    private static void Main()
    {
        delegate* unmanaged<int> genericInt = &Script.Int;
        delegate* unmanaged<float> genericFloat = &Script.Float;
        delegate* unmanaged<int> genericForward = &Script.Forwarded;
        delegate* unmanaged<int> genericBounce = &Script.Bounced;
        delegate* unmanaged<int> genericPair = &Script.Pair;
        delegate* unmanaged<int> genericRefOut = &Script.RefOut;
        delegate* unmanaged<int> genericArray = &Script.Array;
        delegate* unmanaged<int> genericNamedIntFloat = &Script.NamedIntFloat;
        delegate* unmanaged<int> genericNamedFloatInt = &Script.NamedFloatInt;
        delegate* unmanaged<int> genericNamedNested = &Script.NamedNested;
        Console.WriteLine($"generic_int: {genericInt()}");
        Console.WriteLine($"generic_float: {genericFloat()}");
        Console.WriteLine($"generic_forward: {genericForward()}");
        Console.WriteLine($"generic_bounce: {genericBounce()}");
        Console.WriteLine($"generic_pair: {genericPair()}");
        Console.WriteLine($"generic_ref_out: {genericRefOut()}");
        Console.WriteLine($"generic_array: {genericArray()}");
        Console.WriteLine($"generic_named_int_float: {genericNamedIntFloat()}");
        Console.WriteLine($"generic_named_float_int: {genericNamedFloatInt()}");
        Console.WriteLine($"generic_named_nested: {genericNamedNested()}");
    }
}
