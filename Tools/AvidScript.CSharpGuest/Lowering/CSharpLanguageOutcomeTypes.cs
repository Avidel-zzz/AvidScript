using System;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal static class CSharpLanguageOutcomeTypes
{
    internal static string Id(string valueType) => "type:language_outcome:"
        + Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(valueType))).ToLowerInvariant();

    internal static GuestType Create(string valueType)
    {
        GuestField[] fields =
        {
            new("field:status", "status", "type:int32", 0),
            new("field:error_type", "error_type", "type:int32", 0),
            new("field:source", "source", "type:int32", 0),
            new("field:error_root", "error_root", "type:language_error_root", 0),
        };
        return new(Id(valueType), "struct", "memory", valueType == "type:void" ? fields
            : fields.Append(new GuestField("field:value", "value", valueType, 0)).ToArray(), null, null, 0, 1);
    }
}
