using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;

namespace AvidScript.GuestIr;

// IR 24 keeps cancellation distinct from faults while sharing the terminal
// error reader and the frame-rooted object representation.
public static class GuestTaskCancellationErrorValidator
{
    public const int SchemaVersion = 24;
    public const string IrVersion = "1.23";
    public const int SemanticSchemaVersion = 44;
    public const string SemanticVersion = "1.53";
    public const string ImportId = "import:task_cancel_language_error_v1";
    public const string ImportName = "avid_task_cancel_language_error_v1";
    public const string MetaImportId = "import:task_terminal_error_meta_v1";
    public const string MetaImportName = "avid_task_terminal_error_meta_v1";
    public const string RootImportId = "import:task_terminal_error_root_v1";
    public const string RootImportName = "avid_task_terminal_error_root_v1";
    private const string DiagnosticCode = "ASIR1032";

    internal static bool IsVersion(GuestModule module) =>
        module.SchemaVersion == SchemaVersion && module.IrVersion == IrVersion;

    internal static bool Supports(GuestModule module) => IsVersion(module)
        || GuestTaskLocalLifetimeValidator.HasCancellation(module);

    internal static void Validate(GuestValidationContext context)
    {
        GuestModule module = context.Module;
        GuestImport[] cancellation = Find(module, ImportId, ImportName);
        GuestImport[] metadata = Find(module, MetaImportId, MetaImportName);
        GuestImport[] roots = Find(module, RootImportId, RootImportName);
        if (!Supports(module) && cancellation.Length == 0
            && metadata.Length == 0 && roots.Length == 0) return;

        if (!Supports(module) || module.Language != "csharp"
            || module.Provenance.SemanticSchemaVersion != (IsVersion(module) ? SemanticSchemaVersion : GuestTaskLocalLifetimeValidator.ExpectedSemanticSchema(module))
            || module.Provenance.SemanticVersion != (IsVersion(module) ? SemanticVersion : GuestTaskLocalLifetimeValidator.ExpectedSemanticVersion(module))
            || module.LanguageErrorCatalog is not { Types.Count: > 0, Sources.Count: > 0 }
            || cancellation.Length != 1 || metadata.Length != 1 || roots.Length != 1)
        {
            Add(context, "Task cancellation errors require paired Semantic 44/1.53 and IR 24/1.23, a nonempty catalog and the complete cancellation/terminal-read import set.");
            return;
        }
        if (!IsCancellationImport(cancellation[0])
            || !IsImport(metadata[0], MetaImportId, MetaImportName,
                new[] { "type:int64" }, "type:int64")
            || !IsTerminalRootImport(roots[0]))
            Add(context, "Task cancellation imports have a noncanonical identity, module, signature or binding contract.");

        GuestTaskLanguageErrorValidator.ValidateCallTokens(context,
            cancellation[0], module.LanguageErrorCatalog);
        GuestTaskLanguageErrorValidator.ValidateFreshRoots(context, cancellation[0]);
        ValidateCancellationTypes(context, cancellation[0]);
    }

    internal static bool IsCancellationImport(GuestImport import) =>
        IsImport(import, ImportId, ImportName, new[]
            { "type:int64", "type:int32", "type:int32", "type:language_error_root" },
            "type:int32");

    internal static bool IsTerminalRootImport(GuestImport import) =>
        IsImport(import, RootImportId, RootImportName,
            new[] { "type:int64" }, "type:language_error_root");

    private static bool IsImport(GuestImport import, string id, string name,
        IReadOnlyList<string> parameters, string returnType) =>
        import.Id == id && import.Name == name
        && import.Module == GuestTaskResultValidator.ImportModule
        && import.ParameterTypeIds.SequenceEqual(parameters, StringComparer.Ordinal)
        && import.ReturnTypeId == returnType && import.DispatchClass == "semantic"
        && import.OptimizationClass == "none" && import.BindingOrdinal == -1;

    private static GuestImport[] Find(GuestModule module, string id, string name) =>
        module.Imports.Where(import => import.Id == id || import.Name == name).ToArray();

    private static void ValidateCancellationTypes(GuestValidationContext context,
        GuestImport cancellation)
    {
        HashSet<int> allowed = context.Module.LanguageErrorCatalog!.Types
            .Where(type => type.TypeId is "type:global::System.OperationCanceledException"
                or "type:global::System.Threading.Tasks.TaskCanceledException")
            .Select(type => type.Token).ToHashSet();
        foreach (GuestFunction function in context.Module.Functions)
        foreach (GuestBasicBlock block in function.Blocks)
        for (int index = 0; index < block.Instructions.Count; ++index)
        {
            GuestInstruction call = block.Instructions[index];
            if (call.Op != "call" || call.TargetId != cancellation.Id) continue;
            // Both tokens must be defined before this call, in the same block.
            // The shared token validator also rejects mutable/ambiguous values.
            bool LiteralBefore(string id, out int token)
            {
                token = 0;
                GuestInstruction? literal = block.Instructions.Take(index)
                    .FirstOrDefault(instruction => instruction.ResultId == id);
                return literal is { Op: "constant", Constant.Kind: "int32" }
                    && int.TryParse(literal.Constant.Value, NumberStyles.None,
                        CultureInfo.InvariantCulture, out token);
            }
            if (call.OperandIds.Count != 4
                || !LiteralBefore(call.OperandIds[1], out int typeToken)
                || !allowed.Contains(typeToken)
                || !LiteralBefore(call.OperandIds[2], out _))
                Add(context, $"Function '{function.Id}' must cancel with literal catalog tokens for TaskCanceledException or OperationCanceledException defined before the call.");
        }
    }

    private static void Add(GuestValidationContext context, string message) =>
        context.Add(DiagnosticCode, message);
}
