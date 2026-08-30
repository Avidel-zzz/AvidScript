namespace AvidScript;

[AvidStateContract(AvidStateMode.Explicit)]
public static class DelegateRefOutScript
{
    [AvidEvent(AvidEvents.OnRefOutSignal)]
    public static void HandleRefOutSignal(ref int value, out int doubled)
    {
        value += 3;
        doubled = value * 2;
    }
}
