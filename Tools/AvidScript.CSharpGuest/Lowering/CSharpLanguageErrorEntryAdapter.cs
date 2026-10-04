using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

// Keeps a UE export's ABI while the source function returns an internal outcome.
internal static class CSharpLanguageErrorEntryAdapter
{
    internal const string ImportModule = "avidscript";
    internal const string ImportName = "avid_language_error_report_v1";
    private const string ImportId = "import:language_error_report_v1";

    internal static bool TryAdd(GuestModule module,
        IReadOnlyList<GuestExport> affectedExports,
        IReadOnlyDictionary<string, GuestFunction> originalFunctions,
        out GuestModule? adapted, out string? error, bool publishExports = true)
    {
        adapted = null;
        error = null;
        if (affectedExports.Count == 0) { adapted = module; return true; }
        GuestImport reportImport = CSharpTaskResultAbi.LanguageErrorReportImport();
        var existing = module.Imports.Where(import => import.Id == ImportId
            || import.Module == ImportModule && import.Name == ImportName).ToArray();
        // Async entry lowering may already own this same boundary import. Reuse
        // its complete ABI; aliases, duplicates and signature collisions still fail.
        if (existing.Length > 1 || existing.Length == 1 && (existing[0].Id != reportImport.Id
            || existing[0].Module != reportImport.Module || existing[0].Name != reportImport.Name
            || existing[0].ReturnTypeId != reportImport.ReturnTypeId
            || !existing[0].ParameterTypeIds.SequenceEqual(reportImport.ParameterTypeIds)))
        {
            error = "The language-error report import identity is already occupied.";
            return false;
        }
        List<GuestFunction> functions = module.Functions.ToList();
        List<GuestExport> exports = module.Exports.ToList();
        foreach (GuestExport export in affectedExports)
        {
            if (!originalFunctions.TryGetValue(export.FunctionId, out GuestFunction? original)
                || !module.Functions.Any(function => function.Id == export.FunctionId))
            {
                error = $"Export '{export.Name}' needs a supported UE boundary adapter.";
                return false;
            }
            GuestFunction target = module.Functions.Single(function => function.Id == export.FunctionId);
            GuestLanguageOutcomeType? outcome = module.LanguageOutcomeTypes?.SingleOrDefault(
                entry => entry.TypeId == target.ReturnTypeId);
            if (outcome is null || outcome.ValueTypeId !=
                (original.ReturnTypeId == "type:void" ? null : original.ReturnTypeId))
            {
                error = $"Export '{export.Name}' has no matching language-outcome return type.";
                return false;
            }
            string id = "function:language_error_entry:" + export.Name;
            if (functions.Any(function => function.Id == id))
            {
                error = $"Adapter identity for '{export.Name}' is already occupied.";
                return false;
            }
            GuestRegister[] parameters = original.Parameters.ToArray();
            List<GuestRegister> locals = new()
            {
                new("entry:outcome", target.ReturnTypeId),
                new("entry:status", "type:int32"),
                new("entry:error_type", "type:int32"),
                new("entry:source", "type:int32"),
                new("entry:error_root", "type:language_error_root"),
                new("entry:report_result", "type:int32"),
            };
            bool hasValue = original.ReturnTypeId != "type:void";
            if (hasValue) locals.Add(new GuestRegister("entry:value", original.ReturnTypeId));
            functions.Add(new GuestFunction(id, parameters, locals, original.ReturnTypeId, "entry", new[]
            {
                new GuestBasicBlock("entry", new[]
                {
                    new GuestInstruction("call", "entry:outcome",
                        parameters.Select(parameter => parameter.Id).ToArray(), target.Id, null, null),
                    new GuestInstruction("field_load", "entry:status",
                        new[] { "entry:outcome" }, "field:status", null, null),
                }, new GuestTerminator("branch_if", "entry:status", "error", "success", null)),
                new GuestBasicBlock("error", new[]
                {
                    new GuestInstruction("field_load", "entry:error_type",
                        new[] { "entry:outcome" }, "field:error_type", null, null),
                    new GuestInstruction("field_load", "entry:source",
                        new[] { "entry:outcome" }, "field:source", null, null),
                    new GuestInstruction("field_load", "entry:error_root",
                        new[] { "entry:outcome" }, "field:error_root", null, null),
                    new GuestInstruction("call", "entry:report_result",
                        new[] { "entry:error_type", "entry:source", "entry:error_root" },
                        ImportId, null, null),
                }, new GuestTerminator("trap", null, null, null, null)),
                new GuestBasicBlock("success", hasValue
                        ? new[] { new GuestInstruction("field_load", "entry:value",
                            new[] { "entry:outcome" }, "field:value", null, null) }
                        : Array.Empty<GuestInstruction>(),
                    new GuestTerminator("return", null, null, null,
                        hasValue ? "entry:value" : null)),
            }));
            if (publishExports) exports.Add(new GuestExport(export.Name, id));
        }
        adapted = module with
        {
            Imports = existing.Length == 0 ? module.Imports.Append(reportImport).ToArray() : module.Imports,
            Functions = functions,
            Exports = exports.OrderBy(export => export.Name, StringComparer.Ordinal).ToArray(),
        };
        return true;
    }
}
