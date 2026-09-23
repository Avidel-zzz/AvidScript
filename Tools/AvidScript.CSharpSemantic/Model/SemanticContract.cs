namespace AvidScript.CSharpSemantic;

public static class SemanticContract
{
    public const int CurrentSchemaVersion = 30;
    public const string CurrentSemanticVersion = "1.35";

    public static bool IsCurrentOrPrevious(int schemaVersion, string semanticVersion) =>
        schemaVersion == CurrentSchemaVersion
        && semanticVersion is "1.34" or CurrentSemanticVersion;
}
