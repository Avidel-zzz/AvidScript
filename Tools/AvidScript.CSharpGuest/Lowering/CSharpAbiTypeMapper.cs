using System;
using AvidScript.CSharpSemantic;

namespace AvidScript.CSharpGuest;

internal static class CSharpAbiTypeMapper
{
    public static string ParameterType(SemanticCallableParameter parameter)
    {
        return string.Equals(parameter.RefKind, "none", StringComparison.Ordinal)
            ? parameter.TypeId
            : CSharpGuestIds.AddressTypeId;
    }
}
