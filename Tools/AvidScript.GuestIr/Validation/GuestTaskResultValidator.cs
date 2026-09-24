using System;
using System.Linq;

namespace AvidScript.GuestIr;

// IR 18 owns the scalar Task<int> Host import. Older IR and Semantic artifacts
// must not gain task execution by adding an import to otherwise valid bytes.
internal static class GuestTaskResultValidator
{
    public const int SchemaVersion = 18;
    public const string IrVersion = "1.17";
    public const string ImportModule = "avidscript";
    public const string ImportName = "avid_task_i32_v1";
    public const string BindProducerImportName = "avid_task_bind_producer_v1";
    public const string PropagateFailureImportName = "avid_task_propagate_failure_v1";
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
        bool taskSemantic = module.Provenance.SemanticSchemaVersion == 35
            && module.Provenance.SemanticVersion == "1.44";
        bool taskIr = module.SchemaVersion == SchemaVersion && module.IrVersion == IrVersion;
        if (!taskSemantic && !taskIr && taskImports.Length == 0
            && producerImports.Length == 0 && failureImports.Length == 0) return;

        if (!taskSemantic || !taskIr || module.Language != "csharp"
            || taskImports.Length != 1)
        {
            context.Add(DiagnosticCode,
                "Task<int> requires C# Semantic 35/1.44, Guest IR 18/1.17 and one versioned Host import.");
            return;
        }

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
    }

    private static bool HasScalar(GuestValidationContext context, string id, string storage, int size) =>
        context.Types.TryGetValue(id, out GuestType? type)
        && type.Kind == "scalar" && type.Storage == storage
        && type.Size == size && type.Alignment == size
        && type.Fields.Count == 0 && type.ElementTypeId is null
        && type.UnderlyingTypeId is null;
}
