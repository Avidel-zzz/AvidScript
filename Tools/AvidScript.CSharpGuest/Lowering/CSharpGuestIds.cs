using AvidScript.CSharpSemantic;

namespace AvidScript.CSharpGuest;

internal static class CSharpGuestIds
{
    public const string AddressTypeId = "type:address";
    public const string Int32TypeId = "type:int32";
    public const string Int64TypeId = "type:int64";
    public const string ArrayLengthPropertyId = SemanticIntrinsicIds.ArrayLengthPropertyId;
    public const string GameplayEventExportName = "avid_on_gameplay_event";
    public const string GameplayEventFunctionId = "function:synthetic:gameplay_event";
    public const string ContinuationExportName = "avid_on_continuation";
    public const string ContinuationFunctionId = "function:synthetic:continuation";
    public const string ContinuationV2ExportName = "avid_on_continuation_v2";
    public const string ContinuationV2FunctionId = "function:synthetic:continuation_v2";
    public const string ContinuationStatusTypeId =
        "type:global::AvidScript.AvidContinuationStatus";
    public const string LoadedObjectTypeId =
        "type:global::AvidScript.AvidLoadedObject";
    public const string AsyncResumeFunctionPrefix = "function:synthetic:async_resume:";

    public static string Function(string methodSymbolId) => $"function:{methodSymbolId}";

    public static string Import(string methodSymbolId) => $"import:{methodSymbolId}";

    public static string Global(string fieldSymbolId) => $"global:{fieldSymbolId}";

    public static string Parameter(string symbolId) => $"value:parameter:{symbolId}";

    public static string This(string methodSymbolId) => $"value:this:{methodSymbolId}";

    public static string Local(string symbolId) => $"value:local:{symbolId}";

    public static string AsyncResumeFunction(int callbackId) =>
        $"{AsyncResumeFunctionPrefix}{callbackId}";

    public static string AsyncSegmentBlock(string methodSymbolId, int ordinal) =>
        $"block:synthetic:async_segment:{methodSymbolId}:{ordinal}";

    public static string AsyncResumeParameter(int callbackId, string name) =>
        $"value:async_resume:{callbackId}:parameter:{name}";

    public static string AsyncStateField(string frameTypeId, string symbolId) =>
        $"field:synthetic:async_state:{frameTypeId}:{symbolId}";

    public static string OutcomeStatusField(string outcomeTypeId) =>
        $"field:synthetic:outcome:{outcomeTypeId}:status";

    public static string OutcomeValueField(string outcomeTypeId) =>
        $"field:synthetic:outcome:{outcomeTypeId}:value";

    public static string Temporary(string methodSymbolId, int blockOrdinal, int ordinal) =>
        $"value:temp:{methodSymbolId}:{blockOrdinal}:{ordinal}";

    public static string Capture(string methodSymbolId, string captureId) =>
        $"value:capture:{methodSymbolId}:{captureId}";

    public static string Block(string methodSymbolId, int ordinal) => $"block:{methodSymbolId}:{ordinal}";

    public static string GameplayEventParameter(string name) => $"value:gameplay_event:parameter:{name}";

    public static string GameplayEventLocal(int eventType, string name) =>
        $"value:gameplay_event:{eventType}:{name}";

    public static string GameplayEventCheckBlock(int eventType) =>
        $"block:synthetic:gameplay_event:check:{eventType}";

    public static string GameplayEventCallBlock(int eventType) =>
        $"block:synthetic:gameplay_event:call:{eventType}";

    public const string GameplayEventReturnBlockId = "block:synthetic:gameplay_event:return";

    public static string ContinuationParameter(bool version2, string name) =>
        $"value:{ContinuationPrefix(version2)}:parameter:{name}";

    public static string ContinuationLocal(bool version2, int callbackId, string name) =>
        $"value:{ContinuationPrefix(version2)}:{callbackId}:{name}";

    public static string ContinuationCheckBlock(bool version2, int callbackId) =>
        $"block:synthetic:{ContinuationPrefix(version2)}:check:{callbackId}";

    public static string ContinuationCallBlock(bool version2, int callbackId) =>
        $"block:synthetic:{ContinuationPrefix(version2)}:call:{callbackId}";

    public const string ContinuationReturnBlockId = "block:synthetic:continuation:return";
    public const string ContinuationV2ReturnBlockId = "block:synthetic:continuation_v2:return";

    public static string ContinuationReturnBlock(bool version2) => version2
        ? ContinuationV2ReturnBlockId
        : ContinuationReturnBlockId;

    public const string DelegateEventFunctionPrefix = "function:synthetic:delegate_event:";

    public static string DelegateEventFunction(string subscriptionId) =>
        DelegateEventFunctionPrefix + subscriptionId;

    public static string DelegateEventParameter(string subscriptionId, int ordinal) =>
        $"value:delegate_event:{subscriptionId}:parameter:{ordinal}";

    public static string DelegateEventAggregate(string subscriptionId, int ordinal) =>
        $"value:delegate_event:{subscriptionId}:aggregate:{ordinal}";

    public static string DelegateEventOutputValue(string subscriptionId, int ordinal) =>
        $"value:delegate_event:{subscriptionId}:output_value:{ordinal}";

    public static string DelegateEventOutputAddress(string subscriptionId, int ordinal) =>
        $"value:delegate_event:{subscriptionId}:output_address:{ordinal}";

    public static string DelegateEventOutputOrdinal(string subscriptionId, int ordinal) =>
        $"value:delegate_event:{subscriptionId}:output_ordinal:{ordinal}";

    public static string DelegateEventOutputStatus(string subscriptionId, int ordinal) =>
        $"value:delegate_event:{subscriptionId}:output_status:{ordinal}";

    public static string DelegateEventBlock(string subscriptionId) =>
        $"block:synthetic:delegate_event:{subscriptionId}";

    private static string ContinuationPrefix(bool version2) => version2
        ? "continuation_v2"
        : "continuation";
}
