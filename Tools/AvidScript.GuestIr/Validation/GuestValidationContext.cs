using System;
using System.Collections.Generic;

namespace AvidScript.GuestIr;

internal sealed class GuestValidationContext
{
    private readonly List<GuestDiagnostic> diagnostics;

    public GuestValidationContext(GuestModule module)
    {
        Module = module;
        diagnostics = new List<GuestDiagnostic>(module.Diagnostics);
        Types = new Dictionary<string, GuestType>(StringComparer.Ordinal);
        Imports = new Dictionary<string, GuestImport>(StringComparer.Ordinal);
        Functions = new Dictionary<string, GuestFunction>(StringComparer.Ordinal);
    }

    public GuestModule Module { get; }

    public Dictionary<string, GuestType> Types { get; }

    public Dictionary<string, GuestImport> Imports { get; }

    public Dictionary<string, GuestFunction> Functions { get; }

    public IReadOnlyList<GuestDiagnostic> Diagnostics => diagnostics;

    public bool IsVoidType(string typeId)
    {
        return Types.TryGetValue(typeId, out GuestType? type)
            && string.Equals(type.Kind, "void", StringComparison.Ordinal);
    }

    public void Add(string code, string message)
    {
        diagnostics.Add(new GuestDiagnostic(code, "error", message, null));
    }

    public bool RequireType(string typeId, string owner)
    {
        if (Types.ContainsKey(typeId))
        {
            return true;
        }

        Add("ASIR1003", $"{owner} references unknown type '{typeId}'.");
        return false;
    }
}
