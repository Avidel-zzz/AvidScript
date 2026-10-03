using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.WasmBackend;

namespace AvidScript.CSharpCompilerWorker;

public sealed class CompilerStageExecutor
{
    private static readonly object ConsoleErrorGate = new();
    private readonly SemanticCompilerWorkspace semanticWorkspace = new();

    public CompilerWorkerResponse Execute(
        CompilerWorkerRequest request,
        string workerInstanceId,
        string toolchainFingerprint)
    {
        CompilerWorkerRequestValidator.Validate(request, toolchainFingerprint);
        Stopwatch stopwatch = Stopwatch.StartNew();
        List<string> diagnostics = new();
        int exitCode;
        try
        {
            exitCode = request.Stage switch
            {
                "ping" => 0,
                "shutdown" => 0,
                "frontend" => CaptureDiagnostics(
                    () => ExecuteFrontend(request),
                    diagnostics),
                "semantic" => CaptureDiagnostics(
                    () => ExecuteSemantic(request),
                    diagnostics),
                "guest" => CaptureDiagnostics(
                    () => ExecuteGuest(request),
                    diagnostics),
                _ => throw new InvalidOperationException("Validated compiler stage is unavailable."),
            };
        }
        catch (Exception exception) when (exception is not OutOfMemoryException
            and not StackOverflowException)
        {
            diagnostics.Add($"ASCW2001: {exception.Message}");
            exitCode = 3;
        }
        stopwatch.Stop();
        return new CompilerWorkerResponse
        {
            ProtocolVersion = request.ProtocolVersion,
            RequestId = request.RequestId,
            WorkerInstanceId = workerInstanceId,
            ToolchainFingerprint = toolchainFingerprint,
            Stage = request.Stage,
            Succeeded = exitCode == 0,
            ExitCode = exitCode,
            DurationMs = stopwatch.Elapsed.TotalMilliseconds,
            Diagnostics = NormalizeDiagnostics(diagnostics),
            Workspace = GetWorkspaceMetrics(),
            LanguageProfile = request.LanguageProfile is null ? null : CSharpLanguageProfile.Gameplay.Identity,
        };
    }

    private static int ExecuteFrontend(CompilerWorkerRequest request)
    {
        return FrontendCli.Run(new[]
        {
            "--source", request.SourcePath,
            "--source-id", request.SourceId,
            "--output", request.OutputPath,
        });
    }

    private int ExecuteSemantic(CompilerWorkerRequest request)
    {
        List<string> arguments = new()
        {
            "--source", request.SourcePath,
            "--source-id", request.SourceId,
            "--frontend", request.FrontendPath,
            "--output", request.OutputPath,
        };
        if (!string.IsNullOrWhiteSpace(request.ReferenceSourcePath))
        {
            arguments.Add("--reference-source");
            arguments.Add(request.ReferenceSourcePath);
        }
        if (!string.IsNullOrWhiteSpace(request.ExecutableReferenceSourcePath))
        {
            arguments.Add("--executable-reference-source");
            arguments.Add(request.ExecutableReferenceSourcePath);
        }
        if (request.LanguageProfile is not null)
        {
            arguments.Add("--language-profile");
            arguments.Add(CSharpLanguageProfile.Resolve(request.LanguageProfile.Name).Name);
        }
        return SemanticCommandLine.Run(arguments.ToArray(), semanticWorkspace);
    }

    private static int ExecuteGuest(CompilerWorkerRequest request)
    {
        string debugOffsetPath = request.DebugMapPath + ".offsets.json";
        File.Delete(debugOffsetPath);
        List<string> arguments = new()
        {
            "--semantic", request.SemanticPath,
            "--output", request.GuestIrPath,
            "--state-schema", request.StateSchemaPath,
            "--debug-map", request.DebugMapPath,
            "--frontend-artifact-sha256", request.FrontendArtifactSha256,
            "--data-lane-fusion", request.DataLaneFusion,
            "--debug-instrumentation", request.DebugInstrumentation,
        };
        if (request.LanguageProfile is not null)
        {
            arguments.AddRange(new[] { "--language-profile", CSharpLanguageProfile.Resolve(request.LanguageProfile.Name).Name,
                "--module-id", request.ModuleId! });
        }
        int exitCode = GuestCommandLine.Run(arguments.ToArray());
        if (exitCode != 0)
        {
            return exitCode;
        }

        try
        {
            exitCode = WasmBackendCommandLine.Run(new[]
            {
                request.GuestIrPath,
                request.WasmPath,
                "--debug-offsets",
                debugOffsetPath,
            });
            if (exitCode != 0)
            {
                File.Delete(request.WasmPath);
                File.Delete(request.DebugMapPath);
                return exitCode;
            }

            exitCode = GuestCommandLine.Run(new[]
            {
                "--finalize-debug-map",
                request.DebugMapPath,
                "--offset-map",
                debugOffsetPath,
            });
            if (exitCode != 0)
            {
                File.Delete(request.WasmPath);
                File.Delete(request.DebugMapPath);
                return exitCode;
            }
            exitCode = WasmBackendCommandLine.Run(new[]
            {
                "--inspect",
                request.WasmPath,
                request.InspectionPath,
            });
            if (exitCode != 0)
            {
                File.Delete(request.WasmPath);
                File.Delete(request.DebugMapPath);
                File.Delete(request.InspectionPath);
            }
            return exitCode;
        }
        finally
        {
            File.Delete(debugOffsetPath);
        }
    }

    private static int CaptureDiagnostics(Func<int> action, List<string> diagnostics)
    {
        lock (ConsoleErrorGate)
        {
            TextWriter originalError = Console.Error;
            using StringWriter capturedError = new();
            try
            {
                Console.SetError(capturedError);
                return action();
            }
            finally
            {
                Console.SetError(originalError);
                diagnostics.AddRange(
                    capturedError.ToString().Split(
                        new[] { "\r\n", "\n" },
                        StringSplitOptions.RemoveEmptyEntries));
            }
        }
    }

    private static IReadOnlyList<string> NormalizeDiagnostics(IEnumerable<string> diagnostics)
    {
        return diagnostics
            .Where(item => !string.IsNullOrWhiteSpace(item))
            .Select(item => item.Length <= CompilerWorkerProtocol.MaximumDiagnosticCharacters
                ? item
                : item[..CompilerWorkerProtocol.MaximumDiagnosticCharacters])
            .Take(CompilerWorkerProtocol.MaximumDiagnostics)
            .ToArray();
    }

    private CompilerWorkerWorkspaceMetrics GetWorkspaceMetrics()
    {
        SemanticCompilerWorkspaceSnapshot snapshot = semanticWorkspace.GetSnapshot();
        return new CompilerWorkerWorkspaceMetrics
        {
            MetadataReferenceSetBuilds = snapshot.MetadataReferenceSetBuilds,
            SyntaxTreeCacheHits = snapshot.SyntaxTreeCacheHits,
            SyntaxTreeCacheMisses = snapshot.SyntaxTreeCacheMisses,
            SyntaxTreeCacheEntries = snapshot.SyntaxTreeCacheEntries,
        };
    }
}
