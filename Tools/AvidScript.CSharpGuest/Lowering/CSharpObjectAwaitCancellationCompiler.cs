using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

public static class CSharpObjectAwaitCancellationCompiler
{
    public static bool TryLower(SemanticDocument source, string semanticSha256,
        out GuestModule? module, out string? error)
    {
        module = null;
        error = "Object await cancellation requires a validated paired source contract.";
        if (!CSharpObjectAwaitExecutionContext.TryCreate(source, out var execution, out var context)) return false;
        GuestModule candidate;
        if (source.StaticInitialization is not null)
        {
            if (!CSharpStaticInitializationCompiler.TryLower(execution!, semanticSha256, out var composed, out error)) return false;
            candidate = composed!;
        }
        else
        {
            if (SemanticContract.HasCancellationTokens(source))
            {
                if (!CSharpCancellationTokenExecutionContext.TryCreate(execution!, out var tokenSource, out _)) return false;
                context!.Attach(tokenSource!);
                execution = tokenSource;
            }
            if (!CSharpLanguageErrorCompiler.TryLower(execution!, semanticSha256, out var compiled,
                    out error, deferComposedValidation: true)) return false;
            candidate = context!.Wrap(compiled!.Module);
        }
        var validation = GuestModuleValidator.Validate(candidate);
        if (!validation.Succeeded)
        {
            error = "Object await cancellation failed independent execution validation: "
                + string.Join(" | ", validation.Diagnostics.Select(diagnostic => diagnostic.Code + ": " + diagnostic.Message));
            return false;
        }
        module = candidate;
        error = null;
        return true;
    }
}
