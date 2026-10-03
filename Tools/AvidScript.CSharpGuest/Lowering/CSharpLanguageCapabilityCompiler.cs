using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

// Select a validated source compiler, never reinterpret the input version.
// Ordinary lowering remains a separate public admission boundary.
public static class CSharpLanguageCapabilityCompiler
{
    public static bool IsRequired(SemanticDocument source) => source.StaticInitialization is not null
        || source.CapabilityManifest is not null
        || source.AsyncMethods?.Any(method => method?.VoidErrorOwner is not null) == true
        || source.SchemaVersion is SemanticContract.AsyncSynchronousExceptionSchemaVersion
            or SemanticContract.AsyncCatchVariableSchemaVersion or SemanticContract.CancellationTokenSchemaVersion
            or SemanticComposableCapabilities.SchemaVersion or SemanticContract.AsyncVoidErrorOwnerSchemaVersion
            or SemanticComposableCapabilities.AsyncVoidSchemaVersion or SemanticComposableCapabilities.StaticAsyncValueSchemaVersion
        || source.SemanticVersion is SemanticContract.AsyncSynchronousExceptionSemanticVersion
            or SemanticContract.AsyncCatchVariableSemanticVersion or SemanticContract.CancellationTokenSemanticVersion
            or SemanticComposableCapabilities.SemanticVersion or SemanticContract.AsyncVoidErrorOwnerSemanticVersion
            or SemanticComposableCapabilities.AsyncVoidSemanticVersion or SemanticComposableCapabilities.StaticAsyncValueSemanticVersion;

    public static bool TryLower(SemanticDocument source, string semanticSha256,
        out GuestModule? module, out string? error)
    {
        module = null;
        error = "Unsupported or incomplete language capability source contract.";
        if (!IsRequired(source)) return false;
        // Static preparation owns field rewriting and attaches private execution
        // contexts before composing the remaining source capabilities.
        if (source.StaticInitialization is not null)
            return CSharpStaticInitializationCompiler.TryLower(source, semanticSha256, out module, out error);
        if (!SemanticContract.IsCurrentOrPrevious(source.SchemaVersion, source.SemanticVersion)) return false;
        if (SemanticContract.HasCancellationTokens(source))
            return CSharpCancellationTokenCompiler.TryLower(source, semanticSha256, out module, out error);
        if (SemanticContract.HasAsyncVoidErrorOwner(source) || SemanticContract.HasAsyncSynchronousExceptions(source))
        {
            if (!CSharpLanguageErrorCompiler.TryLower(source, semanticSha256, out var compilation, out error)) return false;
            module = compilation!.Module;
            return true;
        }
        return false;
    }
}
