using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AvidScript.GuestIr;

public static class GuestExceptionValues
{
    public const int SchemaVersion = 33;
    public const string IrVersion = "1.32";
    public const int SemanticSchemaVersion = 52;
    public const string SemanticVersion = "1.61";
    public const string RootImportId = GuestTaskCancellationErrorValidator.RootImportId;
    public const string CapturePrefix = "value:$caught:";

    public static bool IsVersion(GuestModule module) =>
        module.SchemaVersion == SchemaVersion && module.IrVersion == IrVersion;

    // A private validation/execution view. Serialized provenance keeps the
    // original source contract; the outer validator checks it before admission.
    public static GuestModule BaseProfile(GuestModule module) => IsVersion(module)
        && module.ExceptionValues is not null
        && module.Provenance.SemanticSchemaVersion == SemanticSchemaVersion
        && module.Provenance.SemanticVersion == SemanticVersion
        ? module with {
            SchemaVersion = GuestTaskCancellationIdentity.SchemaVersion,
            IrVersion = GuestTaskCancellationIdentity.IrVersion,
            Provenance = module.Provenance with {
                SemanticSchemaVersion = GuestAsyncSynchronousExceptions.SemanticSchemaVersion,
                SemanticVersion = GuestAsyncSynchronousExceptions.SemanticVersion,
            },
        } : module;
}

[JsonUnmappedMemberHandling(JsonUnmappedMemberHandling.Disallow)]
public sealed record GuestExceptionValuePlan(
    [property: JsonPropertyOrder(0), JsonRequired] IReadOnlyList<GuestExceptionValueBinding> Bindings);

[JsonUnmappedMemberHandling(JsonUnmappedMemberHandling.Disallow)]
public sealed record GuestExceptionValueBinding(
    [property: JsonPropertyOrder(0), JsonRequired] string MethodFunctionId,
    [property: JsonPropertyOrder(1), JsonRequired] string FunctionId,
    [property: JsonPropertyOrder(2), JsonRequired] string BlockId,
    [property: JsonPropertyOrder(3), JsonRequired] string OwnerLocalId,
    [property: JsonPropertyOrder(4), JsonRequired] string VariableLocalId,
    [property: JsonPropertyOrder(5), JsonRequired] string ReferenceTypeId);
