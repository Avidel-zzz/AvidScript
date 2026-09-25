using System;
using System.Runtime.InteropServices;

namespace AvidScript;

public static class BoundedLanguageErrorsLifecycle
{
    private static void Validate(int value)
    {
        if (value < 0)
        {
            throw new Exception();
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static void BeginPlay()
    {
        try
        {
            Validate(-1);
        }
        catch (Exception)
        {
            Validate(1);
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds)
    {
        if (deltaSeconds < 0)
        {
            Validate(-1);
        }
        else
        {
            Validate(1);
        }
    }
}
