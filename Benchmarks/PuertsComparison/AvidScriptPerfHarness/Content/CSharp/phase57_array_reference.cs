using System;

namespace AvidScript;

[AttributeUsage(AttributeTargets.Method, Inherited = false, AllowMultiple = false)]
public sealed class AvidExportAttribute : Attribute
{
    public AvidExportAttribute(string entryPoint)
    {
        EntryPoint = entryPoint;
    }

    public string EntryPoint { get; }
}
