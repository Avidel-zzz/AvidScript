namespace AvidScript.CSharpGuest;

public static class CSharpGuestDebugProbeAbi
{
    public const int Version = 1;
    public const string ImportId = "import:synthetic:debug_probe:v1";
    public const string ModuleName = "avidscript";
    public const string ImportName = "avid_debug_probe";
    public const string ProbeIdTypeId = "type:int64";
    public const string ActionTypeId = "type:int32";

    public const int ContinueAction = 0;
    public const int PauseAction = 1;
    public const int AbortAction = 2;
}
