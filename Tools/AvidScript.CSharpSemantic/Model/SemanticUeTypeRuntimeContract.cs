using System;
using System.Security.Cryptography;
using System.Text;

namespace AvidScript.CSharpSemantic;

public static class SemanticUeTypeRuntimeContract
{
    public const string ExportPrefix = "avid_ue_";

    public static string GetFunctionExportName(string methodSymbolId)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(methodSymbolId);
        string hash = Convert.ToHexString(
            SHA256.HashData(Encoding.UTF8.GetBytes(methodSymbolId))).ToLowerInvariant();
        return ExportPrefix + hash[..32];
    }
}
