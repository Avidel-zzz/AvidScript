using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace AvidScript.CSharpSemantic;

[JsonUnmappedMemberHandling(JsonUnmappedMemberHandling.Disallow)]
public sealed record CSharpLanguageProfileIdentity(
    [property: JsonRequired] string Name,
    [property: JsonRequired] string ContractSha256);

public sealed class CSharpLanguageProfile
{
    private static readonly string[] AnalysisNames = {
        "--async-cancellation-flow", "--async-catch-variables", "--async-exception-flow",
        "--async-synchronous-exceptions", "--async-void-error-owner", "--cancellation-tokens",
        "--direct-await-cleanup", "--static-initialization" };
    public static CSharpLanguageProfile Gameplay { get; } = new();
    public string Name => "gameplay-v1";
    public string LanguageErrors => "bounded";
    public string DataLaneFusion => "enabled";
    public string DebugInstrumentation => "disabled";
    public string CanonicalJson { get; }
    public CSharpLanguageProfileIdentity Identity { get; }

    private CSharpLanguageProfile()
    {
        using var stream = new MemoryStream();
        using (var writer = new Utf8JsonWriter(stream))
        {
            writer.WriteStartObject();
            writer.WriteNumber("schema_version", 1);
            writer.WriteString("name", Name);
            writer.WriteString("source_language", "csharp");
            writer.WriteStartObject("analysis");
            foreach (string option in AnalysisNames) writer.WriteBoolean(option[2..], true);
            writer.WriteEndObject();
            writer.WriteStartObject("execution");
            writer.WriteString("data_lane_fusion", DataLaneFusion);
            writer.WriteString("debug_instrumentation", DebugInstrumentation);
            writer.WriteString("language_errors", LanguageErrors);
            writer.WriteEndObject();
            writer.WriteEndObject();
        }
        byte[] bytes = stream.ToArray();
        CanonicalJson = Encoding.UTF8.GetString(bytes);
        Identity = new(Name, Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant());
    }

    public static CSharpLanguageProfile Resolve(string name) => name == Gameplay.Name
        ? Gameplay : throw new ArgumentException($"Unsupported C# language profile: {name}");

    public bool Enables(string analysisOption) => Array.IndexOf(AnalysisNames, analysisOption) >= 0
        ? true : throw new ArgumentException($"Unknown profile analysis option: {analysisOption}");

    public static CSharpLanguageProfile? FromSemanticOptions(IReadOnlyDictionary<string, string> options)
    {
        if (!options.TryGetValue("--language-profile", out string? name)) return null;
        CSharpLanguageProfile profile = Resolve(name);
        if (AnalysisNames.Any(options.ContainsKey))
            throw new ArgumentException("A language profile cannot be combined with explicit semantic analysis options.");
        return profile;
    }

    public static CSharpLanguageProfile? FromGuestOptions(IReadOnlyDictionary<string, string> options)
    {
        if (!options.TryGetValue("--language-profile", out string? name)) return null;
        CSharpLanguageProfile profile = Resolve(name);
        foreach (var expected in new[] { ("--language-errors", profile.LanguageErrors),
            ("--data-lane-fusion", profile.DataLaneFusion), ("--debug-instrumentation", profile.DebugInstrumentation) })
            if (options.TryGetValue(expected.Item1, out string? value) && value != expected.Item2)
                throw new ArgumentException($"{expected.Item1} conflicts with language profile {profile.Name}.");
        return profile;
    }

    public string DescribeJson()
    {
        using JsonDocument definition = JsonDocument.Parse(CanonicalJson);
        return JsonSerializer.Serialize(new { name = Name, contract_sha256 = Identity.ContractSha256,
            definition = definition.RootElement });
    }
}
