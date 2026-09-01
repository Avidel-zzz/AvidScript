using System;
using System.IO;
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

        if (args.Length != 2)
        {
            Console.Error.WriteLine(
                "Usage: avidscript-wasm-backend <input.guest.json> <output.wasm> | --inspect <input.wasm> <output.json>");
            return 2;
        }

        try
        {
            GuestModule module = GuestIrSerializer.Deserialize(File.ReadAllBytes(args[0]));
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
}
