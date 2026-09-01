using System;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace AvidScript.CSharpCompilerWorker;

public sealed record CompilerWorkerServerOptions(
    string PipeName,
    string ToolchainFingerprint,
    TimeSpan IdleTimeout);

public sealed class CompilerWorkerServer
{
    private readonly CompilerWorkerServerOptions options;
    private readonly CompilerStageExecutor executor = new();
    private readonly string workerInstanceId = Guid.NewGuid().ToString("N");

    public CompilerWorkerServer(CompilerWorkerServerOptions options)
    {
        ArgumentNullException.ThrowIfNull(options);
        if (string.IsNullOrWhiteSpace(options.PipeName)
            || options.PipeName.Length > 200)
        {
            throw new ArgumentException("Compiler worker pipe name is invalid.", nameof(options));
        }
        if (options.IdleTimeout < TimeSpan.FromSeconds(1)
            || options.IdleTimeout > TimeSpan.FromHours(1))
        {
            throw new ArgumentOutOfRangeException(
                nameof(options),
                "Compiler worker idle timeout must be between one second and one hour.");
        }
        CompilerWorkerRequestValidator.Validate(
            new CompilerWorkerRequest
            {
                ProtocolVersion = CompilerWorkerProtocol.Version,
                RequestId = "server-options",
                ToolchainFingerprint = options.ToolchainFingerprint,
                Stage = "ping",
            },
            options.ToolchainFingerprint);
        this.options = options;
    }

    public async Task<int> RunAsync(CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            using NamedPipeServerStream pipe = new(
                options.PipeName,
                PipeDirection.InOut,
                1,
                PipeTransmissionMode.Byte,
                PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
            {
                using CancellationTokenSource idleCancellation =
                    CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
                idleCancellation.CancelAfter(options.IdleTimeout);
                try
                {
                    await pipe.WaitForConnectionAsync(idleCancellation.Token).ConfigureAwait(false);
                }
                catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
                {
                    return 0;
                }
            }

            bool shouldShutdown = await ServeConnectionAsync(
                pipe,
                cancellationToken).ConfigureAwait(false);
            if (shouldShutdown)
            {
                return 0;
            }
        }
        return 0;
    }

    private async Task<bool> ServeConnectionAsync(
        NamedPipeServerStream pipe,
        CancellationToken cancellationToken)
    {
        using StreamReader reader = new(
            pipe,
            new UTF8Encoding(false, true),
            detectEncodingFromByteOrderMarks: false,
            bufferSize: 4096,
            leaveOpen: true);
        using StreamWriter writer = new(
            pipe,
            new UTF8Encoding(false),
            bufferSize: 4096,
            leaveOpen: true)
        {
            AutoFlush = true,
            NewLine = "\n",
        };

        string requestId = string.Empty;
        string stage = string.Empty;
        CompilerWorkerResponse response;
        bool shouldShutdown = false;
        Stopwatch stopwatch = Stopwatch.StartNew();
        try
        {
            string? line = await reader.ReadLineAsync(cancellationToken).ConfigureAwait(false);
            if (line is null || line.Length > CompilerWorkerProtocol.MaximumMessageCharacters)
            {
                throw new InvalidDataException("ASCW1009: Compiler worker request size is invalid.");
            }
            CompilerWorkerRequest request = CompilerWorkerJson.DeserializeRequest(line);
            requestId = request.RequestId;
            stage = request.Stage;
            response = executor.Execute(
                request,
                workerInstanceId,
                options.ToolchainFingerprint);
            shouldShutdown = request.Stage == "shutdown" && response.Succeeded;
        }
        catch (Exception exception) when (exception is InvalidDataException
            or JsonException
            or ArgumentException
            or IOException)
        {
            stopwatch.Stop();
            response = new CompilerWorkerResponse
            {
                RequestId = requestId,
                WorkerInstanceId = workerInstanceId,
                ToolchainFingerprint = options.ToolchainFingerprint,
                Stage = stage,
                Succeeded = false,
                ExitCode = 2,
                DurationMs = stopwatch.Elapsed.TotalMilliseconds,
                Diagnostics = new[] { $"ASCW1010: {exception.Message}" },
                Workspace = new CompilerWorkerWorkspaceMetrics(),
            };
        }

        string responseJson = CompilerWorkerJson.SerializeResponse(response);
        await writer.WriteLineAsync(responseJson.AsMemory(), cancellationToken).ConfigureAwait(false);
        return shouldShutdown;
    }
}
