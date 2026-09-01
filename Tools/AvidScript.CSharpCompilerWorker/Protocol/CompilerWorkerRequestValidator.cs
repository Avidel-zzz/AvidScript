using System;
using System.Collections.Generic;
using System.IO;
using System.Text.RegularExpressions;

namespace AvidScript.CSharpCompilerWorker;

public static partial class CompilerWorkerRequestValidator
{
    private static readonly HashSet<string> Stages = new(StringComparer.Ordinal)
    {
        "ping",
        "shutdown",
        "frontend",
        "semantic",
        "guest",
    };

    public static void Validate(CompilerWorkerRequest request, string expectedToolchainFingerprint)
    {
        ArgumentNullException.ThrowIfNull(request);
        if (request.ProtocolVersion != CompilerWorkerProtocol.Version)
        {
            throw new ArgumentException("ASCW1001: Compiler worker protocol version is unsupported.");
        }
        if (!RequestIdPattern().IsMatch(request.RequestId))
        {
            throw new ArgumentException("ASCW1002: Compiler worker request_id is invalid.");
        }
        if (!Sha256Pattern().IsMatch(request.ToolchainFingerprint)
            || !request.ToolchainFingerprint.Equals(
                expectedToolchainFingerprint,
                StringComparison.Ordinal))
        {
            throw new ArgumentException("ASCW1003: Compiler worker toolchain fingerprint differs.");
        }
        if (!Stages.Contains(request.Stage))
        {
            throw new ArgumentException("ASCW1004: Compiler worker stage is unsupported.");
        }

        switch (request.Stage)
        {
            case "ping":
            case "shutdown":
                return;
            case "frontend":
                RequirePath(request.SourcePath, nameof(request.SourcePath));
                RequireSourceId(request.SourceId);
                RequirePath(request.OutputPath, nameof(request.OutputPath));
                return;
            case "semantic":
                RequirePath(request.SourcePath, nameof(request.SourcePath));
                RequireSourceId(request.SourceId);
                RequirePath(request.FrontendPath, nameof(request.FrontendPath));
                RequirePath(request.OutputPath, nameof(request.OutputPath));
                RequireOptionalPath(request.ReferenceSourcePath, nameof(request.ReferenceSourcePath));
                RequireOptionalPath(
                    request.ExecutableReferenceSourcePath,
                    nameof(request.ExecutableReferenceSourcePath));
                return;
            case "guest":
                RequirePath(request.SemanticPath, nameof(request.SemanticPath));
                RequirePath(request.GuestIrPath, nameof(request.GuestIrPath));
                RequirePath(request.DebugMapPath, nameof(request.DebugMapPath));
                RequirePath(request.StateSchemaPath, nameof(request.StateSchemaPath));
                RequirePath(request.WasmPath, nameof(request.WasmPath));
                RequirePath(request.InspectionPath, nameof(request.InspectionPath));
                if (!Sha256Pattern().IsMatch(request.FrontendArtifactSha256))
                {
                    throw new ArgumentException(
                        "ASCW1005: Guest stage requires a lowercase Frontend artifact SHA-256.");
                }
                if (request.DataLaneFusion is not ("enabled" or "disabled"))
                {
                    throw new ArgumentException(
                        "ASCW1006: Guest stage data_lane_fusion must be enabled or disabled.");
                }
                return;
        }
    }

    private static void RequireSourceId(string value)
    {
        if (string.IsNullOrWhiteSpace(value)
            || value.Length > 1024
            || Path.IsPathRooted(value)
            || value.Contains("..", StringComparison.Ordinal))
        {
            throw new ArgumentException("ASCW1007: Compiler worker source_id is invalid.");
        }
    }

    private static void RequirePath(string value, string name)
    {
        if (string.IsNullOrWhiteSpace(value)
            || value.Length > 32768
            || !Path.IsPathRooted(value))
        {
            throw new ArgumentException($"ASCW1008: Compiler worker path is invalid: {name}.");
        }
    }

    private static void RequireOptionalPath(string value, string name)
    {
        if (!string.IsNullOrWhiteSpace(value))
        {
            RequirePath(value, name);
        }
    }

    [GeneratedRegex("^[A-Za-z0-9._-]{1,128}$", RegexOptions.CultureInvariant)]
    private static partial Regex RequestIdPattern();

    [GeneratedRegex("^[0-9a-f]{64}$", RegexOptions.CultureInvariant)]
    private static partial Regex Sha256Pattern();
}
