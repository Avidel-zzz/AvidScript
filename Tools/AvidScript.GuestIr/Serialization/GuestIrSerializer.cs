using System;
using System.IO;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace AvidScript.GuestIr;

public static class GuestIrSerializer
{
    private static readonly JsonSerializerOptions Options = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
        PropertyNameCaseInsensitive = false,
        DefaultIgnoreCondition = JsonIgnoreCondition.Never,
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
        WriteIndented = true,
    };

    public static byte[] Serialize(GuestModule module)
    {
        ArgumentNullException.ThrowIfNull(module);

        byte[] json = JsonSerializer.SerializeToUtf8Bytes(module, Options);
        byte[] artifact = new byte[json.Length + 1];
        json.CopyTo(artifact, 0);
        artifact[^1] = (byte)'\n';
        return artifact;
    }

    public static GuestModule Deserialize(ReadOnlySpan<byte> artifact)
    {
        try
        {
            return JsonSerializer.Deserialize<GuestModule>(artifact, Options)
                ?? throw new InvalidDataException("Guest IR artifact contains JSON null.");
        }
        catch (JsonException exception)
        {
            throw new InvalidDataException("Guest IR artifact is not valid schema JSON.", exception);
        }
    }
}
