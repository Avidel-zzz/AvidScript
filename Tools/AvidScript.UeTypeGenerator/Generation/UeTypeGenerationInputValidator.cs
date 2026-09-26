using System;
using System.Linq;
using AvidScript.CSharpSemantic;

namespace AvidScript.UeTypeGenerator;

internal static class UeTypeGenerationInputValidator
{
    public static SemanticDocument Validate(SemanticDocument document, bool allowBoundedLanguageErrors)
    {
        bool legacyException = document.SchemaVersion == SemanticContract.ExceptionFlowSchemaVersion
            && document.SemanticVersion == SemanticContract.ExceptionFlowSemanticVersion;
        if ((!legacyException && !SemanticContract.IsCurrentOrPrevious(document.SchemaVersion, document.SemanticVersion))
            || document.Diagnostics is null || document.AsyncMethods is null
            || !SemanticExceptionFlowContractValidator.IsValid(document)
            || !SemanticAsyncInvocationValidator.IsValid(document))
            throw new InvalidOperationException("Semantic artifact has an unsupported version or invalid Task/exception lifetime contract.");

        bool syncErrors = document.ExceptionFlows is { Count: > 0 };
        bool asyncErrors = document.AsyncMethods.Any(method => method.ErrorPlan is not null || method.ExceptionPlan is not null);
        var errors = document.Diagnostics.Where(diagnostic => diagnostic.Severity == "error").ToArray();
        string expectedDiagnostic = syncErrors ? "ASCS3001" : "ASCS5422";
        if ((syncErrors || asyncErrors) && !allowBoundedLanguageErrors
            || document.Succeeded && errors.Length != 0
            || !document.Succeeded && (!allowBoundedLanguageErrors || !(syncErrors || asyncErrors)
                || errors.Length == 0 || errors.Any(diagnostic => diagnostic.Code != expectedDiagnostic)))
            throw new InvalidOperationException("Bounded language errors require a validated exception-flow artifact without unrelated errors.");

        // The legacy synchronous exception contract predates the method catalog
        // reader's version range. Only its declaration view uses the base schema;
        // the manifest always hashes and identifies the untouched source artifact.
        // Task contracts keep their original version and complete lifetime plans.
        return legacyException ? document with
        {
            SchemaVersion = SemanticContract.CurrentSchemaVersion,
            SemanticVersion = SemanticContract.CurrentSemanticVersion,
            Succeeded = true,
            ExceptionFlows = null,
            Diagnostics = document.Diagnostics.Where(diagnostic => diagnostic.Severity != "error").ToArray(),
        } : document;
    }
}
