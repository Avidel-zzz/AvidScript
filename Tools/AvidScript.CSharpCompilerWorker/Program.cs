using System;
using System.Collections.Generic;
using System.Globalization;
using System.Threading;
using System.Threading.Tasks;

namespace AvidScript.CSharpCompilerWorker;

internal static class Program
{
    private static async Task<int> Main(string[] args)
    {
        try
        {
            IReadOnlyDictionary<string, string> options = ParseOptions(args);
            string pipeName = GetRequired(options, "--pipe");
            string toolchainFingerprint = GetRequired(options, "--toolchain-fingerprint");
            int idleTimeoutSeconds = int.Parse(
                GetRequired(options, "--idle-timeout-seconds"),
                NumberStyles.None,
                CultureInfo.InvariantCulture);
            CompilerWorkerServer server = new(new CompilerWorkerServerOptions(
                pipeName,
                toolchainFingerprint,
                TimeSpan.FromSeconds(idleTimeoutSeconds)));
            return await server.RunAsync(CancellationToken.None).ConfigureAwait(false);
        }
        catch (Exception exception) when (exception is ArgumentException
            or FormatException
            or OverflowException)
        {
            Console.Error.WriteLine(exception.Message);
            return 2;
        }
    }

    private static IReadOnlyDictionary<string, string> ParseOptions(string[] args)
    {
        if (args.Length != 6)
        {
            throw new ArgumentException(
                "Usage: --pipe <name> --toolchain-fingerprint <sha256> --idle-timeout-seconds <seconds>");
        }
        Dictionary<string, string> options = new(StringComparer.Ordinal);
        for (int index = 0; index < args.Length; index += 2)
        {
            if (string.IsNullOrWhiteSpace(args[index + 1])
                || !options.TryAdd(args[index], args[index + 1]))
            {
                throw new ArgumentException("Compiler worker options must be unique and non-empty.");
            }
        }
        foreach (string name in options.Keys)
        {
            if (name is not ("--pipe" or "--toolchain-fingerprint" or "--idle-timeout-seconds"))
            {
                throw new ArgumentException($"Unknown compiler worker option: {name}");
            }
        }
        return options;
    }

    private static string GetRequired(IReadOnlyDictionary<string, string> options, string name)
    {
        return options.TryGetValue(name, out string? value)
            ? value
            : throw new ArgumentException($"Missing compiler worker option: {name}");
    }
}
