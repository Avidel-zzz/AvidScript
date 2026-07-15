using System;
using System.IO;
using System.Text.Json;
using AvidScript.WasmBackend;

internal static class WasmArtifactInspectionReportTests
{
    public static int Run()
    {
        PublishedArtifactInspectionReadsActualExports();
        return 1;
    }

    private static void PublishedArtifactInspectionReadsActualExports()
    {
        string root = Path.Combine(Path.GetTempPath(), "AvidScriptWasmInspection", Guid.NewGuid().ToString("N"));
        string wasmPath = Path.Combine(root, "sample.wasm");
        string reportPath = Path.Combine(root, "sample.wasm.inspect.json");
        Directory.CreateDirectory(root);

        try
        {
            File.WriteAllBytes(wasmPath, CreateExportedFunctionModule());
            WasmArtifactInspectionReport.WriteJson(wasmPath, reportPath);

            using JsonDocument document = JsonDocument.Parse(File.ReadAllBytes(reportPath));
            JsonElement rootElement = document.RootElement;
            Assert(rootElement.GetProperty("schema_version").GetInt32() == 1,
                "inspection report should use schema v1");
            Assert(rootElement.GetProperty("sha256").GetString()?.Length == 64,
                "inspection report should hash the inspected file");
            JsonElement exports = rootElement.GetProperty("exports");
            Assert(exports.GetArrayLength() == 1
                && exports[0].GetProperty("name").GetString() == "guest"
                && exports[0].GetProperty("kind").GetByte() == 0,
                "inspection report should expose the actual function export");
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    private static byte[] CreateExportedFunctionModule()
    {
        return new byte[]
        {
            0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
            0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
            0x03, 0x02, 0x01, 0x00,
            0x07, 0x09, 0x01, 0x05, 0x67, 0x75, 0x65, 0x73, 0x74, 0x00, 0x00,
            0x0a, 0x04, 0x01, 0x02, 0x00, 0x0b,
        };
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
