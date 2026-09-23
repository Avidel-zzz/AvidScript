using System;
using System.Collections.Generic;
using System.Security.Cryptography;
using System.Text;

namespace AvidScript.CSharpSemantic;

public static class SemanticContract
{
    public const int CurrentSchemaVersion = 30;
    public const string CurrentSemanticVersion = "1.36";

    public static string GenericInstanceId(string definitionId, IReadOnlyList<string> arguments)
    {
        string key = definitionId + "\n" + string.Join("\n", arguments);
        string digest = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(key))).ToLowerInvariant();
        return "symbol:generic_instance:" + digest;
    }

    public static bool IsCurrentOrPrevious(int schemaVersion, string semanticVersion) =>
        schemaVersion == CurrentSchemaVersion
        && semanticVersion is "1.34" or "1.35" or CurrentSemanticVersion;
}
