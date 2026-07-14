namespace AvidScript.CSharpGuest;

internal static class CSharpGuestIds
{
    public const string AddressTypeId = "type:address";

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
}
