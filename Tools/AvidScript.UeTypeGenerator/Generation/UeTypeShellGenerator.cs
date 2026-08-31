using System;
using System.Collections.Generic;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using AvidScript.CSharpSemantic;

namespace AvidScript.UeTypeGenerator;

public static class UeTypeShellGenerator
{
    public const int ManifestSchemaVersion = 5;
    public const string GeneratorVersion = "1.6";
    public const string ManifestPath = "AvidScriptGeneratedManifest.json";

    public static UeTypeGenerationResult Generate(
        ReadOnlySpan<byte> semanticArtifact,
        string moduleName,
        string unrealVersion)
    {
        ValidateIdentity(moduleName, nameof(moduleName));
        if (string.IsNullOrWhiteSpace(unrealVersion)
            || unrealVersion.Any(character => char.IsControl(character)))
        {
            throw new ArgumentException("Unreal version must be non-empty and contain no control characters.", nameof(unrealVersion));
        }

        byte[] artifact = semanticArtifact.ToArray();
        SemanticDocument document = SemanticSerializer.Deserialize(artifact);
        IReadOnlyList<UeTypeManifestEntry> types = UeTypeGenerationPlanner.Plan(
            document,
            moduleName);
        string artifactHash = Hash(artifact);
        string generationKey = Hash(Encoding.UTF8.GetBytes(string.Join("\n", new[]
        {
            GeneratorVersion,
            document.SchemaVersion.ToString(System.Globalization.CultureInfo.InvariantCulture),
            document.SemanticVersion,
            artifactHash,
            moduleName,
            unrealVersion,
            string.Empty,
        })));
        Dictionary<string, byte[]> generatedSources = new(StringComparer.Ordinal)
        {
            [UhtShellRenderer.HeaderPath] = Encode(UhtShellRenderer.RenderHeader(moduleName, types)),
            [UhtShellRenderer.SourcePath] = Encode(UhtShellRenderer.RenderSource(types)),
        };
        UeGeneratedFileEntry[] outputs = generatedSources
            .OrderBy(pair => pair.Key, StringComparer.Ordinal)
            .Select(pair => new UeGeneratedFileEntry(pair.Key, Hash(pair.Value), pair.Value.Length))
            .ToArray();
        UeTypeGenerationManifest manifest = new(
            ManifestSchemaVersion,
            GeneratorVersion,
            document.SchemaVersion,
            document.SemanticVersion,
            artifactHash,
            generationKey,
            moduleName,
            unrealVersion,
            types,
            outputs);
        generatedSources.Add(ManifestPath, UeTypeManifestSerializer.Serialize(manifest));
        return new UeTypeGenerationResult(manifest, generatedSources);
    }

    private static void ValidateIdentity(string value, string parameterName)
    {
        if (value.Length == 0
            || !(char.IsAsciiLetter(value[0]) || value[0] == '_')
            || value.Skip(1).Any(character => !(char.IsAsciiLetterOrDigit(character) || character == '_')))
        {
            throw new ArgumentException("Module name must be an ASCII C++ identifier.", parameterName);
        }
    }

    private static byte[] Encode(string value)
    {
        return new UTF8Encoding(false).GetBytes(value);
    }

    private static string Hash(ReadOnlySpan<byte> value)
    {
        return Convert.ToHexString(SHA256.HashData(value)).ToLowerInvariant();
    }
}
