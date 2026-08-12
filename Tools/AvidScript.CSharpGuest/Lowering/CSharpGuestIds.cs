namespace AvidScript.CSharpGuest;

internal static class CSharpGuestIds
{
    public const string AddressTypeId = "type:address";
    public const string Int32TypeId = "type:int32";
    public const string ArrayLengthPropertyId =
        "symbol:property:global::System.Array.Length:int32";
    public const string GameplayEventExportName = "avid_on_gameplay_event";
    public const string GameplayEventFunctionId = "function:synthetic:gameplay_event";

    public static string Function(string methodSymbolId) => $"function:{methodSymbolId}";

    public static string Import(string methodSymbolId) => $"import:{methodSymbolId}";

    public static string Global(string fieldSymbolId) => $"global:{fieldSymbolId}";

    public static string Parameter(string symbolId) => $"value:parameter:{symbolId}";

    public static string This(string methodSymbolId) => $"value:this:{methodSymbolId}";

    public static string Local(string symbolId) => $"value:local:{symbolId}";

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

    public const string DelegateEventFunctionPrefix = "function:synthetic:delegate_event:";

    public static string DelegateEventFunction(string subscriptionId) =>
        DelegateEventFunctionPrefix + subscriptionId;

    public static string DelegateEventParameter(string subscriptionId, int ordinal) =>
        $"value:delegate_event:{subscriptionId}:parameter:{ordinal}";

    public static string DelegateEventAggregate(string subscriptionId, int ordinal) =>
        $"value:delegate_event:{subscriptionId}:aggregate:{ordinal}";

    public static string DelegateEventBlock(string subscriptionId) =>
        $"block:synthetic:delegate_event:{subscriptionId}";
}
