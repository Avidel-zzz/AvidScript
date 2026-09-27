using System.Linq;
using System.Text.Json.Serialization;

namespace AvidScript.GuestIr;

public static class GuestCancellationTokens
{
    public const int SchemaVersion = 34;
    public const string IrVersion = "1.33";
    public const int SemanticSchemaVersion = 53;
    public const string SemanticVersion = "1.62";
    public const string TypeId = "type:global::System.Threading.CancellationToken";
    public const string FieldId = "field:$cancellation_token:identity";
    public const string ReadImportId = "import:exception_cancellation_token_v1";
    public const string ReadImportName = "avid_exception_cancellation_token_v1";
    public const string RootTypeId = "type:language_error_root";

    public static GuestType ValueType() => new(TypeId, "struct", "memory",
        new[] { new GuestField(FieldId, "identity", "type:int64", 0) }, null, null, 8, 8);
    public static GuestImport Reader() => new(ReadImportId, "avidscript", ReadImportName,
        new[] { RootTypeId }, "type:int64");

    public static bool IsVersion(GuestModule module) => module.SchemaVersion == SchemaVersion && module.IrVersion == IrVersion;
    public static bool IsBase(int schema, string version) => (schema, version) is (14, "1.13") or (17, "1.16") or (29, "1.28");

    public static bool HasReaderProfile(GuestModule module) => IsVersion(module) && module.Language == "csharp"
        && module.Provenance.SemanticSchemaVersion == SemanticSchemaVersion && module.Provenance.SemanticVersion == SemanticVersion
        && module.CancellationTokens is { } plan && plan.BaseSchemaVersion != 14 && IsBase(plan.BaseSchemaVersion, plan.BaseIrVersion);

    public static bool IsReader(GuestImport import) => import is {
        Id: ReadImportId, Module: "avidscript", Name: ReadImportName, ReturnTypeId: "type:int64",
        DispatchClass: "semantic", OptimizationClass: "none", BindingOrdinal: -1,
    } && import.ParameterTypeIds.SequenceEqual(new[] { RootTypeId });

    // Retain all ownership metadata. Its independent validators still inspect
    // each outer contract; this view only selects the underlying instruction set.
    public static GuestModule BaseProfile(GuestModule module)
    {
        if (!IsVersion(module) || module.CancellationTokens is not { } plan
            || !IsBase(plan.BaseSchemaVersion, plan.BaseIrVersion)
            || module.Provenance.SemanticSchemaVersion != SemanticSchemaVersion
            || module.Provenance.SemanticVersion != SemanticVersion) return module;
        (int schema, string version, int sourceSchema, string sourceVersion) = plan.BaseSchemaVersion switch
        {
            14 => (14, "1.13", 31, "1.40"),
            17 => (17, "1.16", 34, "1.43"),
            _ when module.ExceptionValues is not null => (33, "1.32", 52, "1.61"),
            _ when module.CancellationIdentity is not null => (32, "1.31", 50, "1.59"),
            _ => (29, "1.28", 50, "1.59"),
        };
        return module with {
            SchemaVersion = schema, IrVersion = version,
            Provenance = module.Provenance with { SemanticSchemaVersion = sourceSchema, SemanticVersion = sourceVersion },
        };
    }
}

[JsonUnmappedMemberHandling(JsonUnmappedMemberHandling.Disallow)]
public sealed record GuestCancellationTokenPlan(
    [property: JsonPropertyOrder(0), JsonRequired] int BaseSchemaVersion,
    [property: JsonPropertyOrder(1), JsonRequired] string BaseIrVersion);
