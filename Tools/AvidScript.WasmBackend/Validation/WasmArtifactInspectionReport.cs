using System;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text.Json;

namespace AvidScript.WasmBackend;

public static class WasmArtifactInspectionReport
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
    };

    public static void WriteJson(string wasmPath, string reportPath)
    {
        string fullWasmPath = Path.GetFullPath(wasmPath);
        string fullReportPath = Path.GetFullPath(reportPath);
        byte[] artifact = File.ReadAllBytes(fullWasmPath);
        WasmArtifactInfo inspection = WasmArtifactInspector.Inspect(artifact);
        object report = new
        {
            schema_version = 1,
            wasm_file = fullWasmPath,
            sha256 = Convert.ToHexString(SHA256.HashData(artifact)).ToLowerInvariant(),
            exports = inspection.Exports.Select(item => new
            {
                name = item.Name,
                kind = item.Kind,
                index = item.Index,
            }).ToArray(),
            imports = inspection.Imports.Select(item => new
            {
                module = item.Module,
                name = item.Name,
                kind = item.Kind,
                type_index = item.TypeIndex,
            }).ToArray(),
        };

        Directory.CreateDirectory(Path.GetDirectoryName(fullReportPath)!);
        string temporaryPath = $"{fullReportPath}.tmp.{Environment.ProcessId}";
        try
        {
            File.WriteAllBytes(temporaryPath, JsonSerializer.SerializeToUtf8Bytes(report, JsonOptions));
            File.Move(temporaryPath, fullReportPath, overwrite: true);
        }
        finally
        {
            if (File.Exists(temporaryPath))
            {
                File.Delete(temporaryPath);
            }
        }
    }
}
