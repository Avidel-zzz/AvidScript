namespace AvidScript.GuestIr;

// Exact composition of static storage with the synchronous-to-Task error
// contract. The old static envelope must not reinterpret this execution model.
public static class GuestStaticAsyncExecution
{
    public const int SchemaVersion = 30;
    public const string IrVersion = "1.29";
    public const int SemanticSchemaVersion = 51;
    public const string SemanticVersion = "1.60";

    public static bool IsVersion(GuestModule module) =>
        module.SchemaVersion == SchemaVersion && module.IrVersion == IrVersion;

    internal static bool HasSourceContract(GuestModule module) =>
        module.Provenance.SemanticSchemaVersion == SemanticSchemaVersion
        && module.Provenance.SemanticVersion == SemanticVersion
        && module.StaticStorage is { BaseSchemaVersion: GuestAsyncSynchronousExceptions.SchemaVersion,
            BaseIrVersion: GuestAsyncSynchronousExceptions.IrVersion }
        && module.AsyncSynchronousExceptions is not null;
}
