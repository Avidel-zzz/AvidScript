using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace AvidScript.CSharpFrontend;

public static class FrontendCli
{
    private const string Usage =
        "Usage: AvidScript.CSharpFrontend --source <path> --source-id <id> --output <path>";

    public static int Run(string[] args)
    {
        if (!TryParseArguments(args, out CliOptions options, out string error))
        {
            Console.Error.WriteLine(error);
            Console.Error.WriteLine(Usage);
            return 2;
        }

        try
        {
            string source = File.ReadAllText(
                options.SourcePath,
                new UTF8Encoding(encoderShouldEmitUTF8Identifier: false, throwOnInvalidBytes: true));
            FrontendDocument document = FrontendAnalyzer.Analyze(source, options.SourceId);
            AtomicFileWriter.Write(options.OutputPath, FrontendSerializer.Serialize(document));
            return document.Succeeded ? 0 : 1;
        }
        catch (Exception exception) when (
            exception is IOException or UnauthorizedAccessException or ArgumentException)
        {
            Console.Error.WriteLine(exception.Message);
            return 2;
        }
    }

    private static bool TryParseArguments(
        string[] args,
        out CliOptions options,
        out string error)
    {
        options = null!;
        error = string.Empty;
        Dictionary<string, string> values = new(StringComparer.Ordinal);

        for (int index = 0; index < args.Length; index++)
        {
            string name = args[index];
            if (name is not ("--source" or "--source-id" or "--output"))
            {
                error = $"Unknown argument: {name}";
                return false;
            }

            if (values.ContainsKey(name))
            {
                error = $"Duplicate argument: {name}";
                return false;
            }

            if (index + 1 >= args.Length || args[index + 1].StartsWith("--", StringComparison.Ordinal))
            {
                error = $"Missing value for argument: {name}";
                return false;
            }

            values.Add(name, args[++index]);
        }

        foreach (string required in new[] { "--source", "--source-id", "--output" })
        {
            if (!values.TryGetValue(required, out string? value) || string.IsNullOrEmpty(value))
            {
                error = $"Missing required argument: {required}";
                return false;
            }
        }

        options = new CliOptions(
            values["--source"],
            values["--source-id"],
            values["--output"]);
        return true;
    }

    private sealed record CliOptions(string SourcePath, string SourceId, string OutputPath);
}

internal static class AtomicFileWriter
{
    public static void Write(string outputPath, byte[] contents)
    {
        string fullOutputPath = Path.GetFullPath(outputPath);
        string? directory = Path.GetDirectoryName(fullOutputPath);
        if (directory is null)
        {
            throw new ArgumentException("Output path must identify a file.", nameof(outputPath));
        }

        Directory.CreateDirectory(directory);
        string temporaryPath = Path.Combine(
            directory,
            $".{Path.GetFileName(fullOutputPath)}.{Guid.NewGuid():N}.tmp");

        try
        {
            using (FileStream stream = new(
                temporaryPath,
                FileMode.CreateNew,
                FileAccess.Write,
                FileShare.None,
                bufferSize: 4096,
                FileOptions.WriteThrough))
            {
                stream.Write(contents);
                stream.Flush(flushToDisk: true);
            }

            File.Move(temporaryPath, fullOutputPath, overwrite: true);
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
