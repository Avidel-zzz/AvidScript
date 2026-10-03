using System;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

// Public CLI and build admission share the same source compiler selection.
public static class CSharpGuestCompiler
{
    public static CSharpGuestLoweringResult Compile(SemanticDocument document, string semanticSha256,
        bool dataLaneFusionEnabled = true, bool debugInstrumentationEnabled = false,
        bool boundedLanguageErrors = false, string? requestedModuleId = null)
    {
        ArgumentNullException.ThrowIfNull(document);
        ArgumentNullException.ThrowIfNull(semanticSha256);
        bool capabilityCompilerRequired = CSharpLanguageCapabilityCompiler.IsRequired(document);
        bool asyncLanguageErrors = document.SchemaVersion == SemanticContract.AsyncLanguageErrorSchemaVersion
                && document.AsyncMethods.Any(method => method.ErrorPlan is not null)
            || document.SchemaVersion is (SemanticContract.AsyncExceptionFlowSchemaVersion
                or SemanticContract.DirectAwaitCleanupSchemaVersion or SemanticContract.AsyncCancellationFlowSchemaVersion
                or SemanticContract.TaskLocalLifetimeSchemaVersion or SemanticContract.AsyncThrowRoutingSchemaVersion)
                && document.AsyncMethods.Any(method => method.ExceptionPlan is not null);
        if (boundedLanguageErrors && (document.ExceptionFlows is { Count: > 0 }
                || asyncLanguageErrors || capabilityCompilerRequired)
            && (!dataLaneFusionEnabled || debugInstrumentationEnabled))
            throw new ArgumentException("Bounded language errors require data-lane fusion enabled and debug instrumentation disabled.");

        GuestModule? module;
        if (capabilityCompilerRequired)
        {
            module = null;
            string? error = null;
            if (!boundedLanguageErrors || !CSharpLanguageCapabilityCompiler.TryLower(document, semanticSha256,
                    out module, out error) || module is null)
                return Failure(!boundedLanguageErrors ? "Language capability execution requires --language-errors bounded."
                    : error ?? "Language capability lowering failed.");
        }
        else if (boundedLanguageErrors && document.ExceptionFlows is { Count: > 0 } && !asyncLanguageErrors)
        {
            if (!CSharpLanguageErrorCompiler.TryLower(document, semanticSha256, out var compilation, out string? error)
                || compilation is null)
                return Failure(error ?? "Bounded language error lowering failed.");
            module = compilation.Module;
        }
        else
        {
            var result = CSharpGuestLowerer.Lower(document, semanticSha256,
                enableDataLaneFusion: dataLaneFusionEnabled, enableDebugInstrumentation: debugInstrumentationEnabled,
                enableAsyncLanguageErrors: boundedLanguageErrors && asyncLanguageErrors);
            if (!result.Succeeded || result.Module is null) return result;
            module = result.Module;
        }
        if (requestedModuleId is not null)
        {
            if (string.IsNullOrWhiteSpace(requestedModuleId) || requestedModuleId.Length > 1024 || requestedModuleId.Any(char.IsControl))
                throw new ArgumentException("--module-id must be at most 1024 characters and contain no control characters.");
            module = module with { ModuleId = requestedModuleId };
        }
        var validation = GuestModuleValidator.Validate(module);
        return validation.Succeeded ? new(true, module, Array.Empty<GuestDiagnostic>())
            : new(false, null, validation.Diagnostics);
    }

    private static CSharpGuestLoweringResult Failure(string message) =>
        new(false, null, new[] { new GuestDiagnostic("ASCG1004", "error", message, null) });
}
