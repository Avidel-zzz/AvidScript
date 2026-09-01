using System;
using System.Security.Cryptography;
using System.Text;

namespace AvidScript.CSharpGuest;

internal static class CSharpGuestDebugProbeIdentity
{
    public const int HexLength = 16;

    public static string Create(
        string moduleId,
        string functionId,
        string semanticOperationId)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(moduleId);
        ArgumentException.ThrowIfNullOrWhiteSpace(functionId);
        ArgumentException.ThrowIfNullOrWhiteSpace(semanticOperationId);

        string identity = string.Create(
            moduleId.Length.ToString().Length
                + functionId.Length.ToString().Length
                + semanticOperationId.Length.ToString().Length
                + moduleId.Length
                + functionId.Length
                + semanticOperationId.Length
                + 6,
            (moduleId, functionId, semanticOperationId),
            static (buffer, state) =>
            {
                int offset = 0;
                offset += WriteComponent(buffer[offset..], state.moduleId);
                offset += WriteComponent(buffer[offset..], state.functionId);
                WriteComponent(buffer[offset..], state.semanticOperationId);
            });
        byte[] digest = SHA256.HashData(Encoding.UTF8.GetBytes(identity));
        return Convert.ToHexString(digest.AsSpan(0, HexLength / 2)).ToLowerInvariant();
    }

    private static int WriteComponent(Span<char> destination, string value)
    {
        string length = value.Length.ToString(System.Globalization.CultureInfo.InvariantCulture);
        length.AsSpan().CopyTo(destination);
        destination[length.Length] = ':';
        value.AsSpan().CopyTo(destination[(length.Length + 1)..]);
        destination[length.Length + 1 + value.Length] = ';';
        return length.Length + value.Length + 2;
    }
}
