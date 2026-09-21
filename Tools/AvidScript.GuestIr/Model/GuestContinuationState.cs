namespace AvidScript.GuestIr;

// Typed instructions retain module-local references; only the backend lowers them
// to the versioned native ABI. The concrete type ordinal is never a Guest operand.
public static class GuestContinuationState
{
    public const int MinimumSchemaVersion = 11;
    public const string StoreOp = "managed_state_store";
    public const string ReadOp = "managed_state_read";
    public const string ImportModule = "avidscript";
    public const string StoreImport = "avid_continuation_state_store_v1";
    public const string ReadImport = "avid_continuation_state_read_v1";
}
