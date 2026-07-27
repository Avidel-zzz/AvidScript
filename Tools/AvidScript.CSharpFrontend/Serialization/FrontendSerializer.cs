using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace AvidScript.CSharpFrontend;

public static class FrontendSerializer
{
    public const int MaximumDepth = 256;

    private static readonly JsonSerializerOptions Options = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
        DefaultIgnoreCondition = JsonIgnoreCondition.Never,
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
        MaxDepth = MaximumDepth,
        WriteIndented = true,
    };

    public static byte[] Serialize(FrontendDocument document)
    {
        byte[] json = JsonSerializer.SerializeToUtf8Bytes(document, Options);
        byte[] terminated = new byte[json.Length + 1];
        json.CopyTo(terminated, 0);
        terminated[^1] = (byte)'\n';
        return terminated;
    }
}
