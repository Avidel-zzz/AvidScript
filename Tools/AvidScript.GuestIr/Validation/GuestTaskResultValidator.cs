using System;
using System.Linq;

namespace AvidScript.GuestIr;

// IR 18 owns the scalar Task<int> Host import. Older IR and Semantic artifacts
// must not gain task execution by adding an import to otherwise valid bytes.
internal static class GuestTaskResultValidator
{
    public const int SchemaVersion = 18;
    public const string IrVersion = "1.17";
    public const int TaskLocalSchemaVersion = 19;
    public const string TaskLocalIrVersion = "1.18";
    public const string ImportModule = "avidscript";
    public const string ImportName = "avid_task_i32_v1";
    public const string BindProducerImportName = "avid_task_bind_producer_v1";
    public const string PropagateFailureImportName = "avid_task_propagate_failure_v1";
    public const string RetainForContinuationImportName = "avid_task_retain_for_continuation_v1";
    private const string DiagnosticCode = "ASIR1028";

    public static void Validate(GuestValidationContext context)
    {
        GuestModule module = context.Module;
        GuestImport[] taskImports = module.Imports.Where(import =>
            import.Module == ImportModule && import.Name == ImportName).ToArray();
        GuestImport[] producerImports = module.Imports.Where(import =>
            import.Module == ImportModule && import.Name == BindProducerImportName).ToArray();
        GuestImport[] failureImports = module.Imports.Where(import =>
            import.Module == ImportModule && import.Name == PropagateFailureImportName).ToArray();
        GuestImport[] retainedImports = module.Imports.Where(import =>
            import.Module == ImportModule && import.Name == RetainForContinuationImportName).ToArray();
        bool taskSemantic = module.Provenance.SemanticSchemaVersion == 35
            && module.Provenance.SemanticVersion == "1.44";
        bool taskIr = module.SchemaVersion == SchemaVersion && module.IrVersion == IrVersion;
        bool taskLocalSemantic = module.Provenance.SemanticSchemaVersion == 36
            && module.Provenance.SemanticVersion == "1.45";
        bool taskAssignmentSemantic = module.Provenance.SemanticSchemaVersion == 37
            && module.Provenance.SemanticVersion == "1.46";
        bool taskExistingLocalSemantic = module.Provenance.SemanticSchemaVersion == 38
            && module.Provenance.SemanticVersion == "1.47";
        bool taskAliasSemantic = module.Provenance.SemanticSchemaVersion == 39
            && module.Provenance.SemanticVersion == "1.48";
        bool taskLocalIr = module.SchemaVersion == TaskLocalSchemaVersion
            && module.IrVersion == TaskLocalIrVersion;
        bool combinedSemantic = module.Provenance.SemanticSchemaVersion
                == GuestTaskLanguageErrorValidator.SemanticSchemaVersion
            && module.Provenance.SemanticVersion == GuestTaskLanguageErrorValidator.SemanticVersion;
        bool combinedIr = module.SchemaVersion == GuestTaskLanguageErrorValidator.SchemaVersion
            && module.IrVersion == GuestTaskLanguageErrorValidator.IrVersion;
        bool asyncErrorSemantic = module.Provenance.SemanticSchemaVersion
                == GuestTaskLanguageErrorValidator.AsyncSemanticSchemaVersion
            && module.Provenance.SemanticVersion == GuestTaskLanguageErrorValidator.AsyncSemanticVersion;
        bool asyncErrorIr = module.SchemaVersion == GuestTaskLanguageErrorValidator.AsyncSchemaVersion
            && module.IrVersion == GuestTaskLanguageErrorValidator.AsyncIrVersion;
        bool exceptionFlowSemantic = module.Provenance.SemanticSchemaVersion
                == GuestTaskLanguageErrorValidator.ExceptionFlowSemanticSchemaVersion
            && module.Provenance.SemanticVersion
                == GuestTaskLanguageErrorValidator.ExceptionFlowSemanticVersion;
        bool exceptionFlowIr = module.SchemaVersion
                == GuestTaskLanguageErrorValidator.ExceptionFlowSchemaVersion
            && module.IrVersion == GuestTaskLanguageErrorValidator.ExceptionFlowIrVersion;
        bool directCleanupSemantic = module.Provenance.SemanticSchemaVersion
                == GuestTaskLanguageErrorValidator.DirectCleanupSemanticSchemaVersion
            && module.Provenance.SemanticVersion
                == GuestTaskLanguageErrorValidator.DirectCleanupSemanticVersion;
        bool directCleanupIr = module.SchemaVersion
                == GuestTaskLanguageErrorValidator.DirectCleanupSchemaVersion
            && module.IrVersion == GuestTaskLanguageErrorValidator.DirectCleanupIrVersion;
        if (!taskSemantic && !taskIr && !taskLocalSemantic && !taskAssignmentSemantic
            && !taskExistingLocalSemantic && !taskAliasSemantic && !taskLocalIr
            && !combinedSemantic && !combinedIr && !asyncErrorSemantic && !asyncErrorIr
            && !exceptionFlowSemantic && !exceptionFlowIr
            && !directCleanupSemantic && !directCleanupIr
            && taskImports.Length == 0 && producerImports.Length == 0
            && failureImports.Length == 0 && retainedImports.Length == 0) return;

        if (!((taskSemantic && taskIr)
            || ((taskLocalSemantic || taskAssignmentSemantic || taskExistingLocalSemantic
                || taskAliasSemantic) && taskLocalIr)
            || (combinedSemantic && combinedIr)
            || (asyncErrorSemantic && asyncErrorIr)
            || (exceptionFlowSemantic && exceptionFlowIr)
            || (directCleanupSemantic && directCleanupIr))
            || module.Language != "csharp"
            || taskImports.Length != 1)
        {
            context.Add(DiagnosticCode,
                "Task<int> requires Semantic 35/1.44 with IR 18/1.17, Semantic 36/1.45 through 39/1.48 with IR 19/1.18, or a paired Task/error version.");
            return;
        }

        if (retainedImports.Length != (taskLocalIr || combinedIr || asyncErrorIr
            || exceptionFlowIr || directCleanupIr ? 1 : 0))
            context.Add(DiagnosticCode,
                "Task continuation retention requires exactly one import in Guest IR 19/1.18 or the combined version, and none in older IR.");

        GuestImport import = taskImports[0];
        if (import.ParameterTypeIds.Count != 4
            || !import.ParameterTypeIds.SequenceEqual(new[]
                { "type:int32", "type:int64", "type:int32", "type:int32" }, StringComparer.Ordinal)
            || import.ReturnTypeId != "type:int64"
            || import.DispatchClass != "semantic" || import.OptimizationClass != "none"
            || import.BindingOrdinal != -1
            || !HasScalar(context, "type:int32", "i32", 4)
            || !HasScalar(context, "type:int64", "i64", 8))
            context.Add(DiagnosticCode, "Task<int> Host import has a noncanonical signature or scalar layout.");

        if (producerImports.Length > 1
            || (producerImports.Length == 1
                && (producerImports[0].ParameterTypeIds.Count != 2
                    || !producerImports[0].ParameterTypeIds.SequenceEqual(
                        new[] { "type:int64", "type:int64" }, StringComparer.Ordinal)
                    || producerImports[0].ReturnTypeId != "type:int32"
                    || producerImports[0].DispatchClass != "semantic"
                    || producerImports[0].OptimizationClass != "none"
                    || producerImports[0].BindingOrdinal != -1)))
            context.Add(DiagnosticCode,
                "Task<int> producer binding import has a noncanonical signature.");

        if (failureImports.Length > 1
            || (failureImports.Length == 1
                && (failureImports[0].ParameterTypeIds.Count != 2
                    || !failureImports[0].ParameterTypeIds.SequenceEqual(
                        new[] { "type:int64", "type:int64" }, StringComparer.Ordinal)
                    || failureImports[0].ReturnTypeId != "type:int32"
                    || failureImports[0].DispatchClass != "semantic"
                    || failureImports[0].OptimizationClass != "none"
                    || failureImports[0].BindingOrdinal != -1)))
            context.Add(DiagnosticCode,
                "Task<int> failure propagation import has a noncanonical signature.");

        if (retainedImports.Length == 1
            && (retainedImports[0].ParameterTypeIds.Count != 2
                || !retainedImports[0].ParameterTypeIds.SequenceEqual(
                    new[] { "type:int64", "type:int64" }, StringComparer.Ordinal)
                || retainedImports[0].ReturnTypeId != "type:int32"
                || retainedImports[0].DispatchClass != "semantic"
                || retainedImports[0].OptimizationClass != "none"
                || retainedImports[0].BindingOrdinal != -1))
            context.Add(DiagnosticCode,
                "Task continuation retention import has a noncanonical signature.");
    }

    private static bool HasScalar(GuestValidationContext context, string id, string storage, int size) =>
        context.Types.TryGetValue(id, out GuestType? type)
        && type.Kind == "scalar" && type.Storage == storage
        && type.Size == size && type.Alignment == size
        && type.Fields.Count == 0 && type.ElementTypeId is null
        && type.UnderlyingTypeId is null;
}
