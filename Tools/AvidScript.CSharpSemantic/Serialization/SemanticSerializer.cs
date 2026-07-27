using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Serialization;
using AvidScript.CSharpFrontend;

namespace AvidScript.CSharpSemantic;

public static class SemanticSerializer
{
    public const int MaximumDepth = FrontendSerializer.MaximumDepth;

    private static readonly JsonSerializerOptions Options = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
        DefaultIgnoreCondition = JsonIgnoreCondition.Never,
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
        MaxDepth = MaximumDepth,
        WriteIndented = true,
    };

    public static byte[] Serialize(SemanticDocument document)
    {
        byte[] json = JsonSerializer.SerializeToUtf8Bytes(document, Options);
        byte[] terminated = new byte[json.Length + 1];
        json.CopyTo(terminated, 0);
        terminated[^1] = (byte)'\n';
        return terminated;
    }
}
