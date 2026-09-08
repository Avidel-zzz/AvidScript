using System;
using System.IO;
using System.Text.Json;
using AvidScript.WasmBackend;

internal static class WasmArtifactInspectionReportTests
{
    public static int Run()
    {
        PublishedArtifactInspectionReadsActualExports();
        CooperativeSafepointReceiptBindsCompilerInputs();
        return 2;
    }

    private static void CooperativeSafepointReceiptBindsCompilerInputs()
    {
        string root = Path.Combine(
            Path.GetTempPath(),
            "AvidScriptSafepointReceipt",
            Guid.NewGuid().ToString("N"));
        string reportPath = Path.Combine(root, "sample.safepoints.json");
        Directory.CreateDirectory(root);

        try
        {
            byte[] guestIrBytes = { 1, 2, 3, 4 };
            byte[] wasmBytes = CreateExportedFunctionModule();
            WasmCooperativeSafepointAttestation attestation = new(
                2,
                64,
                2,
                1,
                3,
                new string('a', 64),
                true);
            WasmCooperativeSafepointAttestationReport.WriteJson(
                reportPath,
                guestIrBytes,
                wasmBytes,
                attestation);

            using JsonDocument document = JsonDocument.Parse(
                File.ReadAllBytes(reportPath));
            JsonElement rootElement = document.RootElement;
            JsonElement proof = rootElement.GetProperty("proof");
            Assert(rootElement.GetProperty("schema_version").GetInt32() == 1
                && rootElement.GetProperty("guest_ir_sha256").GetString()?.Length == 64
                && rootElement.GetProperty("wasm_sha256").GetString()?.Length == 64,
                "safepoint receipt should bind both compiler input and output");
            Assert(proof.GetProperty("schema_version").GetInt32() == 2
                && proof.GetProperty("site_count").GetInt32() == 3
                && proof.GetProperty("site_sha256").GetString() == new string('a', 64)
                && proof.GetProperty("verified").GetBoolean(),
                "safepoint receipt should preserve the verified schema 2 identity");
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
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
