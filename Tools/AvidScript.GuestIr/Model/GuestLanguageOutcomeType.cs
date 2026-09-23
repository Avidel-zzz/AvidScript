using System.Text.Json.Serialization;

namespace AvidScript.GuestIr;

// A versioned layout declaration only. Callable propagation and catch handling
// require separate contracts before source-level exceptions can be enabled.
public sealed record GuestLanguageOutcomeType(
    [property: JsonPropertyOrder(0)] string TypeId,
    [property: JsonPropertyOrder(1)] string? ValueTypeId)
{
    public const int SuccessStatus = 0;
    public const int LanguageErrorStatus = 1;
}
