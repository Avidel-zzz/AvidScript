using System.Collections.Generic;

namespace AvidScript.GuestIr;

public sealed record GuestTypeLayoutResult(
    bool Succeeded,
    IReadOnlyList<GuestType> Types,
    IReadOnlyList<GuestDiagnostic> Diagnostics);

public sealed record GuestDataEncodingResult(
    bool Succeeded,
    GuestDataSegment? Segment,
    IReadOnlyList<GuestDiagnostic> Diagnostics);

public sealed record GuestLayoutResult(
    bool Succeeded,
    GuestMemoryLayout? Layout,
    IReadOnlyList<GuestDataSegment> DataSegments,
    IReadOnlyList<GuestDiagnostic> Diagnostics);
