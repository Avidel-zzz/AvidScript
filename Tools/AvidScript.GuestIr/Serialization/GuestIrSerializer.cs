using System;
using System.Collections.Generic;
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
    private static readonly JsonSerializerOptions StrictOptions = new(Options)
    {
        UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow,
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
            using JsonDocument parsed = JsonDocument.Parse(artifact.ToArray());
            bool strict = false;
            if (parsed.RootElement.ValueKind == JsonValueKind.Object)
                foreach (JsonProperty property in parsed.RootElement.EnumerateObject())
                    if (property.NameEquals("schema_version")
                        && property.Value.ValueKind == JsonValueKind.Number
                        && property.Value.TryGetInt32(out int schema)
                        && schema >= GuestComposableCapabilities.SchemaVersion)
                        strict = true;
            if (strict && !HasUniqueProperties(parsed.RootElement))
                throw new InvalidDataException("Guest IR capability JSON contains duplicate property names.");
            return JsonSerializer.Deserialize<GuestModule>(artifact, strict ? StrictOptions : Options)
                ?? throw new InvalidDataException("Guest IR artifact contains JSON null.");
        }
        catch (JsonException exception)
        {
            throw new InvalidDataException("Guest IR artifact is not valid schema JSON.", exception);
        }
    }

    private static bool HasUniqueProperties(JsonElement element)
    {
        if (element.ValueKind == JsonValueKind.Object)
        {
            HashSet<string> names = new(StringComparer.Ordinal);
            foreach (JsonProperty property in element.EnumerateObject())
                if (!names.Add(property.Name) || !HasUniqueProperties(property.Value))
                    return false;
        }
        else if (element.ValueKind == JsonValueKind.Array)
            foreach (JsonElement item in element.EnumerateArray())
                if (!HasUniqueProperties(item))
                    return false;
        return true;
    }
}
