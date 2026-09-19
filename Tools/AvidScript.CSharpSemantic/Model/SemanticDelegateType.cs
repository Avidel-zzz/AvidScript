using System.Collections.Generic;
using System.Linq;
using System.Text.Json.Serialization;

namespace AvidScript.CSharpSemantic;

public sealed record SemanticDelegateParameter(
    [property: JsonPropertyOrder(0)] int Ordinal,
    [property: JsonPropertyOrder(1)] string TypeId,
    [property: JsonPropertyOrder(2)] string RefKind);

public sealed record SemanticDelegateType(
    [property: JsonPropertyOrder(0)] string TypeId,
    [property: JsonPropertyOrder(1)] string InvokeMethodSymbolId,
    [property: JsonPropertyOrder(2)] string ReturnTypeId,
    [property: JsonPropertyOrder(3)] string ReturnRefKind,
    [property: JsonPropertyOrder(4)] IReadOnlyList<SemanticDelegateParameter> Parameters)
{
    // Closed generic arguments are part of nominal identity. Func<int> and
    // Func<long> must never share an Invoke identity from OriginalDefinition.
    public static string GetInvokeId(string typeId, string returnTypeId, string returnRefKind,
        IReadOnlyList<SemanticDelegateParameter> parameters)
    {
        return $"symbol:delegate_invoke:{typeId}:({string.Join(",", parameters.Select(parameter => parameter.RefKind + ":" + parameter.TypeId))}):{returnRefKind}:{returnTypeId}";
    }
}
