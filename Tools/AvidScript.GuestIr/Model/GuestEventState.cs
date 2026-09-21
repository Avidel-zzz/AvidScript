namespace AvidScript.GuestIr;

// The backend derives concrete state layout ordinals from validated references.
public static class GuestEventState
{
    public const int MinimumSchemaVersion = 12;
    public const string SubscribeOp = "managed_event_subscribe";
    public const string ReadOp = "managed_event_read";
    public const string ImportModule = "avidscript";
    public const string SubscribeImport = "avid_event_state_subscribe_v1";
    public const string ReadImport = "avid_event_state_read_v1";
}
