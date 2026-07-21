namespace AvidScript.CSharpGuest;

internal static class CSharpGuestIds
{
    public const string AddressTypeId = "type:address";
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
}
