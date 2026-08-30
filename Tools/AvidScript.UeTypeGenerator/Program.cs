using System;
using System.IO;

namespace AvidScript.UeTypeGenerator;

internal static class Program
{
    private static int Main(string[] args)
    {
        try
        {
            UeTypeGeneratorCommandLine commandLine = UeTypeGeneratorCommandLine.Parse(args);
            byte[] artifact = File.ReadAllBytes(Path.GetFullPath(commandLine.SemanticPath));
            UeTypeGenerationResult result = UeTypeShellGenerator.Generate(
                artifact,
                commandLine.ModuleName,
                commandLine.UnrealVersion);
            UeTypePublishResult publication = UeTypeGenerationPublisher.Publish(
                commandLine.OutputPath,
                result);
            Console.WriteLine(
                $"AvidScript UE type generation: {result.Manifest.Types.Count} types, "
                + $"{publication.ChangedFileCount} changed files, {publication.CacheHitFileCount} cache hits.");
            return 0;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine("ASUTG1001: " + exception.Message);
            return 1;
        }
    }
}
