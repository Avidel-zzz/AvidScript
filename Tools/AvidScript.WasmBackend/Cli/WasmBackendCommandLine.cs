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

        string? debugOffsetPath = null;
        if (args.Length == 4 && args[2] == "--debug-offsets")
        {
            debugOffsetPath = args[3];
        }
        else if (args.Length != 2)
        {
            Console.Error.WriteLine(
                "Usage: avidscript-wasm-backend <input.guest.json> <output.wasm> [--debug-offsets <output.json>] | --inspect <input.wasm> <output.json>");
            return 2;
        }

        try
        {
            byte[] guestIrArtifact = File.ReadAllBytes(args[0]);
            GuestModule module = GuestIrSerializer.Deserialize(guestIrArtifact);
            WasmCompilationResult result = WasmModuleCompiler.Compile(module);
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
                    module.Imports.Count,
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

    private static string Sha256(ReadOnlySpan<byte> bytes)
    {
        return Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
    }
}
