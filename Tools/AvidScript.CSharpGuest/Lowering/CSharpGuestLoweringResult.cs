using System.Collections.Generic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

public sealed record CSharpGuestLoweringResult(
    bool Succeeded,
    GuestModule? Module,
    IReadOnlyList<GuestDiagnostic> Diagnostics);

internal sealed record CSharpTypeLoweringResult(
    bool Succeeded,
    IReadOnlyList<GuestType> Types,
    IReadOnlyList<GuestDiagnostic> Diagnostics);
