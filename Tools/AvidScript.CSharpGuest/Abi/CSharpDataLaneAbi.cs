namespace AvidScript.CSharpGuest;

internal static class CSharpDataLaneAbi
{
    public const int HeaderBytes = 24;
    public const int CommandBytes = 32;
    public const int MaxCommands = 4096;
    public const int MaxBytes = 256 * 1024;
    public const int MaximumFusedCommands =
        MaxCommands < ((MaxBytes - HeaderBytes) / CommandBytes)
            ? MaxCommands
            : ((MaxBytes - HeaderBytes) / CommandBytes);
}
