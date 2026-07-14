using System;
using System.IO;
using System.Text.Json;
using AvidScript.CSharpSemantic;

namespace AvidScript.CSharpGuest;

internal static class SemanticArtifactReader
{
    private static readonly JsonSerializerOptions Options = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
        PropertyNameCaseInsensitive = false,
    };

    public static SemanticDocument Deserialize(ReadOnlySpan<byte> artifact)
    {
        try
        {
            return JsonSerializer.Deserialize<SemanticDocument>(artifact, Options)
                ?? throw new InvalidDataException("Semantic artifact contains JSON null.");
        }
        catch (JsonException exception)
        {
            throw new InvalidDataException("Semantic artifact is not valid schema JSON.", exception);
        }
    }
}
