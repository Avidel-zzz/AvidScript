using System;
using System.Collections.Generic;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace AvidScript.CSharpCompilerWorker;

public static class CompilerWorkerProtocol
{
    public const int Version = 1;
    public const int MaximumMessageCharacters = 1024 * 1024;
    public const int MaximumDiagnostics = 128;
    public const int MaximumDiagnosticCharacters = 4096;
}

public sealed record CompilerWorkerRequest
{
    public int ProtocolVersion { get; init; }

    public string RequestId { get; init; } = string.Empty;

    public string ToolchainFingerprint { get; init; } = string.Empty;

    public string Stage { get; init; } = string.Empty;

    public string SourcePath { get; init; } = string.Empty;

    public string SourceId { get; init; } = string.Empty;

    public string OutputPath { get; init; } = string.Empty;

    public string FrontendPath { get; init; } = string.Empty;

    public string SemanticPath { get; init; } = string.Empty;

    public string GuestIrPath { get; init; } = string.Empty;

    public string DebugMapPath { get; init; } = string.Empty;

    public string StateSchemaPath { get; init; } = string.Empty;

    public string WasmPath { get; init; } = string.Empty;

    public string InspectionPath { get; init; } = string.Empty;

    public string FrontendArtifactSha256 { get; init; } = string.Empty;

    public string DataLaneFusion { get; init; } = "enabled";

    public string ReferenceSourcePath { get; init; } = string.Empty;

    public string ExecutableReferenceSourcePath { get; init; } = string.Empty;
}

public sealed record CompilerWorkerResponse
{
    public int ProtocolVersion { get; init; } = CompilerWorkerProtocol.Version;

    public string RequestId { get; init; } = string.Empty;

    public string WorkerInstanceId { get; init; } = string.Empty;

    public string ToolchainFingerprint { get; init; } = string.Empty;

    public string Stage { get; init; } = string.Empty;

    public bool Succeeded { get; init; }

    public int ExitCode { get; init; }

    public double DurationMs { get; init; }

    public IReadOnlyList<string> Diagnostics { get; init; } = Array.Empty<string>();
}

public static class CompilerWorkerJson
{
    private static readonly JsonSerializerOptions Options = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
        PropertyNameCaseInsensitive = false,
        UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow,
        WriteIndented = false,
    };

    public static CompilerWorkerRequest DeserializeRequest(string json)
    {
        return JsonSerializer.Deserialize<CompilerWorkerRequest>(json, Options)
            ?? throw new JsonException("Compiler worker request is null.");
    }

    public static CompilerWorkerResponse DeserializeResponse(string json)
    {
        return JsonSerializer.Deserialize<CompilerWorkerResponse>(json, Options)
            ?? throw new JsonException("Compiler worker response is null.");
    }

    public static string SerializeRequest(CompilerWorkerRequest request)
    {
        ArgumentNullException.ThrowIfNull(request);
        return JsonSerializer.Serialize(request, Options);
    }

    public static string SerializeResponse(CompilerWorkerResponse response)
    {
        ArgumentNullException.ThrowIfNull(response);
        return JsonSerializer.Serialize(response, Options);
    }
}
