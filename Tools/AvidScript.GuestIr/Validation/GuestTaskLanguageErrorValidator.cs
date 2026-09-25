using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;

namespace AvidScript.GuestIr;

// IR 20 is the first version that can carry both a language-error catalog and
// Task<int> imports. Paired read imports expose catalog tokens and a frame-rooted
// object; rethrowing after await still requires Guest lowering.
internal static class GuestTaskLanguageErrorValidator
{
    public const int SchemaVersion = 20;
    public const string IrVersion = "1.19";
    public const int SemanticSchemaVersion = 40;
    public const string SemanticVersion = "1.49";
    public const string ImportId = "import:task_fault_language_error_v1";
    public const string ImportName = "avid_task_fault_language_error_v1";
    public const string MetaImportId = "import:task_language_error_meta_v1";
    public const string MetaImportName = "avid_task_language_error_meta_v1";
    public const string RootImportId = "import:task_language_error_root_v1";
    public const string RootImportName = "avid_task_language_error_root_v1";
    private const string DiagnosticCode = "ASIR1029";

    public static void Validate(GuestValidationContext context)
    {
        GuestModule module = context.Module;
        bool combinedVersion = module.SchemaVersion == SchemaVersion && module.IrVersion == IrVersion;
        GuestImport[] imports = module.Imports.Where(import =>
            import.Module == GuestTaskResultValidator.ImportModule && import.Name == ImportName).ToArray();
        GuestImport[] metadataImports = module.Imports.Where(import =>
            import.Name == MetaImportName).ToArray();
        GuestImport[] rootImports = module.Imports.Where(import =>
            import.Name == RootImportName).ToArray();
        if (!combinedVersion && imports.Length == 0
            && metadataImports.Length == 0 && rootImports.Length == 0) return;
        if (!combinedVersion || module.Language != "csharp"
            || module.Provenance.SemanticSchemaVersion != SemanticSchemaVersion
            || module.Provenance.SemanticVersion != SemanticVersion
            || module.LanguageErrorCatalog is null || module.LanguageOutcomeTypes is null
            || imports.Length != 1)
        {
            Add(context, "Task language errors require Semantic 40/1.49, Guest IR 20/1.19, a catalog and exactly one fault import.");
            return;
        }

        GuestImport fault = imports[0];
        if (fault.Id != ImportId
            || !fault.ParameterTypeIds.SequenceEqual(new[]
                { "type:int64", "type:int32", "type:int32", "type:language_error_root" },
                StringComparer.Ordinal)
            || fault.ReturnTypeId != "type:int32"
            || fault.DispatchClass != "semantic" || fault.OptimizationClass != "none"
            || fault.BindingOrdinal != -1
            || !context.Types.TryGetValue("type:language_error_root", out GuestType? root)
            || root.Kind != "managed_ref" || root.Storage != "i64"
            || root.Size != 8 || root.Alignment != 8 || root.ElementTypeId is null)
            Add(context, "Task language-error fault import has a noncanonical signature or root type.");

        string[] requiredTaskImports =
        {
            GuestTaskResultValidator.ImportName,
            GuestTaskResultValidator.BindProducerImportName,
            GuestTaskResultValidator.PropagateFailureImportName,
            GuestTaskResultValidator.RetainForContinuationImportName,
        };
        if (requiredTaskImports.Any(name => module.Imports.Count(import =>
                import.Module == GuestTaskResultValidator.ImportModule && import.Name == name) != 1))
            Add(context, "Combined Task/error IR requires the complete Task<int> import set.");

        if (module.LanguageErrorCatalog is { } catalog)
            ValidateCallTokens(context, fault, catalog);

        if (metadataImports.Length == 0 && rootImports.Length == 0) return;
        if (metadataImports.Length != 1 || rootImports.Length != 1)
        {
            Add(context, "Task language-error read imports must be declared as one metadata/root pair.");
            return;
        }
        if (!IsReadImport(metadataImports[0], MetaImportId, MetaImportName, "type:int64")
            || !IsReadImport(rootImports[0], RootImportId, RootImportName,
                "type:language_error_root"))
            Add(context, "Task language-error read imports have a noncanonical module, signature or binding contract.");
    }

    private static bool IsReadImport(GuestImport import, string id, string name, string returnType) =>
        import.Id == id && import.Module == GuestTaskResultValidator.ImportModule
        && import.Name == name
        && import.ParameterTypeIds.SequenceEqual(new[] { "type:int64" }, StringComparer.Ordinal)
        && import.ReturnTypeId == returnType
        && import.DispatchClass == "semantic" && import.OptimizationClass == "none"
        && import.BindingOrdinal == -1;

    private static void ValidateCallTokens(GuestValidationContext context, GuestImport fault,
        GuestLanguageErrorCatalog catalog)
    {
        HashSet<int> types = catalog.Types.Select(item => item.Token).ToHashSet();
        HashSet<int> sources = catalog.Sources.Select(item => item.Token).ToHashSet();
        Dictionary<string, (string TypeField, string SourceField)> outcomes =
            context.Module.LanguageOutcomeTypes!
                .Where(item => context.Types.TryGetValue(item.TypeId, out GuestType? type)
                    && type.Fields.Count >= 3)
                .ToDictionary(item => item.TypeId, item =>
                {
                    GuestType type = context.Types[item.TypeId];
                    return (type.Fields[1].Id, type.Fields[2].Id);
                }, StringComparer.Ordinal);
        foreach (GuestFunction function in context.Module.Functions)
        {
            Dictionary<string, string> registerTypes = function.Parameters.Concat(function.Locals)
                .GroupBy(register => register.Id, StringComparer.Ordinal)
                .ToDictionary(group => group.Key, group => group.First().TypeId, StringComparer.Ordinal);
            Dictionary<string, GuestInstruction> definitions = new(StringComparer.Ordinal);
            HashSet<string> ambiguous = new(StringComparer.Ordinal);
            foreach (GuestInstruction instruction in function.Blocks.SelectMany(block => block.Instructions))
            {
                if (instruction.ResultId is { } result && !definitions.TryAdd(result, instruction))
                    ambiguous.Add(result);
                if (instruction.Op is "local_store" or "address_of" or "borrow_address"
                    && instruction.TargetId is { } target) ambiguous.Add(target);
            }
            foreach (GuestInstruction call in function.Blocks.SelectMany(block => block.Instructions)
                .Where(instruction => instruction.Op == "call" && instruction.TargetId == fault.Id))
            {
                if (call.OperandIds.Count != 4
                    || !IsCatalogToken(call.OperandIds[1], true, types, outcomes,
                        registerTypes, definitions, ambiguous)
                    || !IsCatalogToken(call.OperandIds[2], false, sources, outcomes,
                        registerTypes, definitions, ambiguous))
                    Add(context, $"Function '{function.Id}' submits an untraceable Task language-error token.");
            }
        }
    }

    private static bool IsCatalogToken(string register, bool isType, HashSet<int> tokens,
        IReadOnlyDictionary<string, (string TypeField, string SourceField)> outcomes,
        IReadOnlyDictionary<string, string> registerTypes,
        IReadOnlyDictionary<string, GuestInstruction> definitions,
        IReadOnlySet<string> ambiguous)
    {
        if (ambiguous.Contains(register) || !definitions.TryGetValue(register, out GuestInstruction? origin))
            return false;
        if (origin.Op == "field_load" && origin.OperandIds.Count == 1
            && registerTypes.TryGetValue(origin.OperandIds[0], out string? ownerType)
            && outcomes.TryGetValue(ownerType, out var fields))
            return origin.TargetId == (isType ? fields.TypeField : fields.SourceField);
        return origin.Op == "constant" && origin.Constant is { Kind: "int32", Value: { } value }
            && int.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out int token)
            && token.ToString(CultureInfo.InvariantCulture) == value && tokens.Contains(token);
    }

    private static void Add(GuestValidationContext context, string message) =>
        context.Add(DiagnosticCode, message);
}
