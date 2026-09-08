using System;
using System.IO;
using System.Security.Cryptography;
using AvidScript.GuestIr;

namespace AvidScript.WasmBackend;

public static class WasmBackendCommandLine
{
    public static int Run(string[] args)
    {
        if (args.Length == 3 && args[0] == "--inspect")
        {
            try
            {
                WasmArtifactInspectionReport.WriteJson(args[1], args[2]);
                return 0;
            }
            catch (Exception exception) when (exception is IOException
                or InvalidDataException
                or UnauthorizedAccessException)
            {
                Console.Error.WriteLine(exception.Message);
                return 1;
            }
        }

        if (args.Length < 2)
        {
            WriteUsage();
            return 2;
        }

        string? debugOffsetPath = null;
        bool enableCooperativeSafepoints = false;
        uint safepointInterval = 256;
        for (int index = 2; index < args.Length; ++index)
        {
            switch (args[index])
            {
                case "--debug-offsets" when index + 1 < args.Length:
                    debugOffsetPath = args[++index];
                    break;
                case "--cooperative-safepoints":
                    enableCooperativeSafepoints = true;
                    break;
                case "--safepoint-interval" when index + 1 < args.Length
                    && uint.TryParse(args[index + 1], out uint parsedInterval):
                    safepointInterval = parsedInterval;
                    enableCooperativeSafepoints = true;
                    ++index;
                    break;
                default:
                    WriteUsage();
                    return 2;
            }
        }

        try
        {
            byte[] guestIrArtifact = File.ReadAllBytes(args[0]);
            GuestModule module = GuestIrSerializer.Deserialize(guestIrArtifact);
            WasmCompilationOptions options = new(
                enableCooperativeSafepoints,
                safepointInterval);
            WasmCompilationResult result = WasmModuleCompiler.Compile(module, options);
            if (!result.Succeeded)
            {
                foreach (WasmDiagnostic diagnostic in result.Diagnostics)
                {
                    Console.Error.WriteLine($"{diagnostic.Code}: {diagnostic.Message}");
                }

                return 1;
            }

            string outputPath = Path.GetFullPath(args[1]);
            Directory.CreateDirectory(Path.GetDirectoryName(outputPath)!);
            File.WriteAllBytes(outputPath, result.Bytes);
            if (debugOffsetPath is not null)
            {
                GuestWasmDebugOffsetMap offsetMap = new(
                    1,
                    module.ModuleId,
                    Sha256(guestIrArtifact),
                    Sha256(result.Bytes),
                    module.Imports.Count + (enableCooperativeSafepoints ? 1 : 0),
                    module.Functions.Count,
                    result.DebugOffsets);
                GuestWasmDebugOffsetMapSerializer.Write(debugOffsetPath, offsetMap);
            }
            return 0;
        }
        catch (Exception exception) when (exception is IOException
            or InvalidDataException
            or UnauthorizedAccessException)
        {
            Console.Error.WriteLine(exception.Message);
            return 1;
        }
    }

    private static void WriteUsage()
    {
        Console.Error.WriteLine(
            "Usage: avidscript-wasm-backend <input.guest.json> <output.wasm> [--debug-offsets <output.json>] [--cooperative-safepoints] [--safepoint-interval <1..65536>] | --inspect <input.wasm> <output.json>");
    }

    private static string Sha256(ReadOnlySpan<byte> bytes)
    {
        return Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
    }
}
