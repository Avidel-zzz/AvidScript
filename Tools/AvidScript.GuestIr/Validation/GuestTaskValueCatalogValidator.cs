using System;
using System.Collections.Generic;

namespace AvidScript.GuestIr;

internal static class GuestTaskValueCatalogValidator
{
    public static void Validate(GuestValidationContext context)
    {
        if (context.InputArtifact.TaskValueCatalog is null) return;
        try { _ = GuestTaskValueCatalog.Encode(context.InputArtifact); }
        catch (Exception error) when (error is ArgumentException or InvalidOperationException or OverflowException or KeyNotFoundException)
        { context.Add("ASIR1081", "Task value catalog is invalid: " + error.Message); }
    }
}
