using System;
using System.Collections.Generic;
using System.IO;
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
    private static readonly JsonSerializerOptions StrictOptions = new(Options)
    {
        UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow,
    };

    public static byte[] Serialize(SemanticDocument document)
    {
        byte[] json = JsonSerializer.SerializeToUtf8Bytes(document, Options);
        byte[] terminated = new byte[json.Length + 1];
        json.CopyTo(terminated, 0);
        terminated[^1] = (byte)'\n';
        return terminated;
    }

    public static SemanticDocument Deserialize(ReadOnlySpan<byte> artifact)
    {
        try
        {
            bool strict = false;
            bool hasManifest = false;
            var reader = new Utf8JsonReader(artifact, new JsonReaderOptions { MaxDepth = MaximumDepth });
            if (reader.Read() && reader.TokenType == JsonTokenType.StartObject)
                while (reader.Read() && reader.TokenType != JsonTokenType.EndObject)
                {
                    if (reader.TokenType != JsonTokenType.PropertyName)
                        throw new JsonException("A Semantic object requires property names.");
                    bool isManifest = reader.ValueTextEquals("capability_manifest");
                    bool isSchema = reader.ValueTextEquals("schema_version");
                    if (!reader.Read()) throw new JsonException("A Semantic property has no value.");
                    if (isManifest) hasManifest = true;
                    if (isSchema && reader.TokenType == JsonTokenType.Number
                        && reader.TryGetInt32(out int schema)
                        && schema >= SemanticComposableCapabilities.SchemaVersion)
                        strict = true;
                    reader.Skip();
                }
            if (hasManifest && !strict)
                throw new InvalidDataException("An older Semantic artifact cannot carry a capability manifest.");
            if (strict)
            {
                using JsonDocument parsed = JsonDocument.Parse(artifact.ToArray());
                if (!HasUniqueProperties(parsed.RootElement))
                    throw new InvalidDataException("Composable Semantic JSON contains duplicate property names.");
            }
            return JsonSerializer.Deserialize<SemanticDocument>(artifact, strict ? StrictOptions : Options)
                ?? throw new InvalidDataException("Semantic artifact contains JSON null.");
        }
        catch (JsonException exception)
        {
            throw new InvalidDataException("Semantic artifact is not valid schema JSON.", exception);
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
