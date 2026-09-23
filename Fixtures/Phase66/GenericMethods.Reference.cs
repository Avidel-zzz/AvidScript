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
        Console.WriteLine($"generic_int: {genericInt()}");
        Console.WriteLine($"generic_float: {genericFloat()}");
        Console.WriteLine($"generic_forward: {genericForward()}");
        Console.WriteLine($"generic_bounce: {genericBounce()}");
        Console.WriteLine($"generic_pair: {genericPair()}");
        Console.WriteLine($"generic_ref_out: {genericRefOut()}");
        Console.WriteLine($"generic_array: {genericArray()}");
    }
}
