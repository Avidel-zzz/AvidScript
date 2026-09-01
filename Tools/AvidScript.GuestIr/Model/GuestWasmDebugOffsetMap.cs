using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace AvidScript.GuestIr;

public sealed record GuestWasmDebugOffsetMap(
    [property: JsonPropertyOrder(0)] int SchemaVersion,
    [property: JsonPropertyOrder(1)] string ModuleId,
    [property: JsonPropertyOrder(2)] string GuestIrSha256,
    [property: JsonPropertyOrder(3)] string WasmSha256,
    [property: JsonPropertyOrder(4)] int ImportedFunctionCount,
    [property: JsonPropertyOrder(5)] int DefinedFunctionCount,
    [property: JsonPropertyOrder(6)] IReadOnlyList<GuestWasmDebugOffset> Offsets);

public sealed record GuestWasmDebugOffset(
    [property: JsonPropertyOrder(0)] int WasmFunctionIndex,
    [property: JsonPropertyOrder(1)] string GuestInstructionId,
    [property: JsonPropertyOrder(2)] int FunctionOffset);

public static class GuestWasmDebugOffsetMapSerializer
{
    private static readonly JsonSerializerOptions Options = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
        PropertyNameCaseInsensitive = false,
        DefaultIgnoreCondition = JsonIgnoreCondition.Never,
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
        WriteIndented = true,
    };

    public static byte[] Serialize(GuestWasmDebugOffsetMap offsetMap)
    {
        ArgumentNullException.ThrowIfNull(offsetMap);
        byte[] json = JsonSerializer.SerializeToUtf8Bytes(offsetMap, Options);
        byte[] artifact = new byte[json.Length + 1];
        json.CopyTo(artifact, 0);
        artifact[^1] = (byte)'\n';
        return artifact;
    }

    public static GuestWasmDebugOffsetMap Deserialize(ReadOnlySpan<byte> artifact)
    {
        try
        {
            return JsonSerializer.Deserialize<GuestWasmDebugOffsetMap>(artifact, Options)
                ?? throw new InvalidDataException("WASM debug offset artifact contains JSON null.");
        }
        catch (JsonException exception)
        {
            throw new InvalidDataException("WASM debug offset artifact is not valid schema JSON.", exception);
        }
    }

    public static void Write(string path, GuestWasmDebugOffsetMap offsetMap)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        ArgumentNullException.ThrowIfNull(offsetMap);

        string fullPath = Path.GetFullPath(path);
        string directory = Path.GetDirectoryName(fullPath) ?? Directory.GetCurrentDirectory();
        Directory.CreateDirectory(directory);
        string temporaryPath = Path.Combine(
            directory,
            $".{Path.GetFileName(fullPath)}.{Guid.NewGuid():N}.tmp");
        try
        {
            File.WriteAllBytes(temporaryPath, Serialize(offsetMap));
            File.Move(temporaryPath, fullPath, overwrite: true);
        }
        finally
        {
            File.Delete(temporaryPath);
        }
    }
}
