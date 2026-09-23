namespace AvidScript.GuestIr;

// The backend derives concrete state layout ordinals from validated references.
public static class GuestEventState
{
    public const int MinimumSchemaVersion = 12;
    public const string SubscribeOp = "managed_event_subscribe";
    public const string ReadOp = "managed_event_read";
    public const int LanguageMinimumSchemaVersion = 14;
    public const string LanguageSubscribeOp = "managed_event_language_subscribe";
    public const string LanguageLookupOp = "managed_event_language_lookup";
    public const string ImportModule = "avidscript";
    public const string SubscribeImport = "avid_event_state_subscribe_v1";
    public const string ReadImport = "avid_event_state_read_v1";
    public const string LanguageSubscribeImport = "avid_event_language_subscribe_v1";
    public const string LanguageLookupImport = "avid_event_language_lookup_v1";
}
