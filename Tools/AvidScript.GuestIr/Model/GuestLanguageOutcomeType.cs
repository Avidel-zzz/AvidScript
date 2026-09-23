using System.Text.Json.Serialization;

namespace AvidScript.GuestIr;

// IR 15 declares layout only. IR 16 additionally validates call-result status
// branches; propagation, cleanup and catch handling still need their own rules.
public sealed record GuestLanguageOutcomeType(
    [property: JsonPropertyOrder(0)] string TypeId,
    [property: JsonPropertyOrder(1)] string? ValueTypeId)
{
    public const int SuccessStatus = 0;
    public const int LanguageErrorStatus = 1;
}
