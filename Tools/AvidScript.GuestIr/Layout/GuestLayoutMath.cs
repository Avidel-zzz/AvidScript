using System;

namespace AvidScript.GuestIr;

internal static class GuestLayoutMath
{
    public const int MaximumAlignment = 16;
    public const int RegionAlignment = 16;
    public const int NullGuardSize = 16;

    public static bool IsValidAlignment(int alignment)
    {
        return alignment > 0
            && alignment <= MaximumAlignment
            && (alignment & (alignment - 1)) == 0;
    }

    public static int AlignUp(int value, int alignment)
    {
        if (value < 0 || !IsValidAlignment(alignment))
        {
            throw new OverflowException("Invalid address or alignment.");
        }

        return checked((value + alignment - 1) & -alignment);
    }

    public static int Add(int left, int right)
    {
        if (left < 0 || right < 0)
        {
            throw new OverflowException("Negative layout size or address.");
        }

        return checked(left + right);
    }

    public static int Multiply(int left, int right)
    {
        if (left < 0 || right < 0)
        {
            throw new OverflowException("Negative layout size.");
        }

        return checked(left * right);
    }
}
