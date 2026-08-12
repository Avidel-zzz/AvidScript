using System.Runtime.InteropServices;

namespace AvidScript;

[AvidStateContract(AvidStateMode.Explicit)]
public static class ArrayRoundtripScript
{
    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static void BeginPlay()
    {
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds)
    {
        AAvidScriptCSharpNameStringTestActor self = UE.Self;
        int[] input = new int[] { 1, 2 };
        int[] inOut = new int[] { 10 };
        int[] result = self.IntArrayRoundTrip(input, ref inOut, out int[] output);
        if (result.Length == 3)
        {
            int sum = 0;
            for (int index = 0; index < result.Length; ++index)
            {
                sum = sum + result[index];
            }
            result[0] = result[0] + 1;
            if (sum == 13 && result[0] == 11)
            {
                self.ReadableIntArray = result;
            }
        }
        AvidScriptValue.Release(inOut);
        AvidScriptValue.Release(output);
        AvidScriptValue.Release(result);
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_timer")]
    public static void OnTimer(int callbackId, int timerHandle)
    {
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_event")]
    public static void OnEvent(int eventId, float value)
    {
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
    public static void EndPlay()
    {
    }
}
