using System.Collections.Generic;
using System.Globalization;
using Microsoft.CodeAnalysis.FlowAnalysis;

namespace AvidScript.CSharpSemantic;

internal sealed class SemanticCaptureRegistry
{
    private readonly Dictionary<CaptureId, string> captureIds = new();

    public string Register(CaptureId captureId)
    {
        if (!captureIds.TryGetValue(captureId, out string? stableId))
        {
            stableId = "capture:" + captureIds.Count.ToString(CultureInfo.InvariantCulture);
            captureIds.Add(captureId, stableId);
        }

        return stableId;
    }
}
