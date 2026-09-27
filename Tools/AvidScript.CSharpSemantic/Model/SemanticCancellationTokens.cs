namespace AvidScript.CSharpSemantic;

// Logical token values are independent of the CLR's private struct layout.
public static class SemanticCancellationTokens
{
    public const string MetadataName = "System.Threading.CancellationToken";
    public const string TypeId = "type:global::" + MetadataName;
    public const string AvidTypeId = "type:global::AvidScript.AvidCancellationToken";
    public const string ExceptionTypeId = "type:global::System.OperationCanceledException";
    public const string None = "cancellation_token_none";
    public const string Read = "cancellation_token_read";
    public const string FromAvid = "cancellation_token_from_avid";
    public const string Compare = "cancellation_token_compare";
    public const string DiagnosticCode = "ASCS5430";
}
