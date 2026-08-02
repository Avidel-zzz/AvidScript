using System.Runtime.InteropServices;

namespace AvidScript;

[AvidStateContract(AvidStateMode.Explicit)]
public static class NameStringRoundtripScript
{
    [AvidTransient]
    private static int Step;

    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static void BeginPlay()
    {
        UAvidScriptCSharpBindingEmitterTestObject self = UE.Self;
        self.ReadableFName = "Input_Name_\u540d";
        self.ReadableFString = "Input_String_\u503c";
        Step = 0;
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds)
    {
        UAvidScriptCSharpBindingEmitterTestObject self = UE.Self;
        if (Step == 0)
        {
            string name = self.ReadableFName;
            string text = self.ReadableFString;
            self.ConstRefFName(name);
            self.ConstRefFString(text);
            self.ReadableFName = name;
            self.ReadableFString = text;
        }
        else if (Step == 1)
        {
            string name = self.ReadableFName;
            string text = self.ReadableFString;
            self.RefFName(ref name);
            self.RefFString(ref text);
            self.ReadableFName = name;
            self.ReadableFString = text;
        }
        else if (Step == 2)
        {
            self.OutFName(out string name);
            self.OutFString(out string text);
            self.ReadableFName = name;
            self.ReadableFString = text;
        }
        else if (Step == 3)
        {
            self.ReadableFName = self.ReturnFName();
            self.ReadableFString = self.ReturnFString();
        }

        Step += 1;
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
