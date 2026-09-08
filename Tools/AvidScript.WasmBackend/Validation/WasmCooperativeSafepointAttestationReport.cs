using System;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text.Json;

namespace AvidScript.WasmBackend;

public static class WasmCooperativeSafepointAttestationReport
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
    };

    public static void WriteJson(
        string reportPath,
        ReadOnlySpan<byte> guestIrBytes,
        ReadOnlySpan<byte> wasmBytes,
        WasmCooperativeSafepointAttestation attestation)
    {
        ArgumentNullException.ThrowIfNull(attestation);
        if (guestIrBytes.IsEmpty
            || wasmBytes.IsEmpty
            || !attestation.Verified
            || attestation.SchemaVersion != 2
            || attestation.PollInterval is 0 or > 65536
            || attestation.LoopPollBlockCount < 0
            || attestation.RecursiveFunctionCount < 0
            || attestation.SiteCount != (long)attestation.LoopPollBlockCount
                + attestation.RecursiveFunctionCount
            || !IsLowercaseSha256(attestation.SiteSha256))
        {
            throw new InvalidDataException(
                "Only verified schema 2 cooperative safepoint attestations can be published.");
        }

        string fullReportPath = Path.GetFullPath(reportPath);
        object report = new
        {
            schema_version = 1,
            guest_ir_sha256 = Sha256(guestIrBytes),
            wasm_sha256 = Sha256(wasmBytes),
            proof = new
            {
                schema_version = attestation.SchemaVersion,
                poll_interval = attestation.PollInterval,
                loop_poll_blocks = attestation.LoopPollBlockCount,
                recursive_functions = attestation.RecursiveFunctionCount,
                site_count = attestation.SiteCount,
                site_sha256 = attestation.SiteSha256,
                verified = attestation.Verified,
            },
        };

        Directory.CreateDirectory(Path.GetDirectoryName(fullReportPath)!);
        string temporaryPath = $"{fullReportPath}.tmp.{Environment.ProcessId}";
        try
        {
            File.WriteAllBytes(
                temporaryPath,
                JsonSerializer.SerializeToUtf8Bytes(report, JsonOptions));
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

    private static string Sha256(ReadOnlySpan<byte> bytes)
    {
        return Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
    }

    private static bool IsLowercaseSha256(string value)
    {
        return value.Length == 64
            && value.All(character => character is >= '0' and <= '9'
                or >= 'a' and <= 'f');
    }
}
