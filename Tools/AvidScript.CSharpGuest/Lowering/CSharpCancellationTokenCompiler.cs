using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

// Explicit while native loading and default-entry acceptance are being completed.
public static class CSharpCancellationTokenCompiler
{
    public static bool TryLower(SemanticDocument source, string semanticSha256,
        out GuestModule? module, out string? error)
    {
        module = null;
        error = null;
        if (source is null || !CSharpCancellationTokenExecutionContext.TryCreate(source, out var execution, out var context)) {
            error = "Cancellation token lowering requires the validated Semantic 53/1.62 contract.";
            return false;
        }
        GuestModule candidate;
        if (context!.NeedsErrors) {
            if (!CSharpLanguageErrorCompiler.TryLower(execution!, semanticSha256, out var lowered, out error)) return false;
            candidate = lowered!.Module;
        } else {
            var lowered = CSharpGuestLowerer.Lower(execution!, semanticSha256);
            if (!lowered.Succeeded || lowered.Module is null) {
                error = string.Join(" | ", lowered.Diagnostics.Select(item => item.Code + ": " + item.Message));
                return false;
            }
            candidate = context.Wrap(lowered.Module);
        }
        var valid = GuestModuleValidator.Validate(candidate);
        if (!valid.Succeeded) {
            error = "Cancellation token execution failed validation: " + string.Join(" | ", valid.Diagnostics.Select(item => item.Code + ": " + item.Message));
            return false;
        }
        module = candidate;
        return true;
    }
}
