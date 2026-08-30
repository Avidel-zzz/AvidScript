using System;
using System.Collections.Generic;

namespace AvidScript.UeTypeGenerator;

internal sealed record UeTypeGeneratorCommandLine(
    string SemanticPath,
    string OutputPath,
    string ModuleName,
    string UnrealVersion)
{
    public static UeTypeGeneratorCommandLine Parse(IReadOnlyList<string> arguments)
    {
        Dictionary<string, string> values = new(StringComparer.Ordinal);
        for (int index = 0; index < arguments.Count; index += 2)
        {
            if (index + 1 >= arguments.Count || !arguments[index].StartsWith("--", StringComparison.Ordinal))
            {
                throw new ArgumentException("Arguments must be provided as --name value pairs.");
            }
            if (!values.TryAdd(arguments[index], arguments[index + 1]))
            {
                throw new ArgumentException($"Argument '{arguments[index]}' was provided more than once.");
            }
        }
        string semantic = Required(values, "--semantic");
        string output = Required(values, "--output");
        string module = Required(values, "--module");
        string unreal = Required(values, "--ue-version");
        if (values.Count != 4)
        {
            throw new ArgumentException("Unknown UE type generator argument.");
        }
        return new UeTypeGeneratorCommandLine(semantic, output, module, unreal);
    }

    private static string Required(IReadOnlyDictionary<string, string> values, string name)
    {
        if (!values.TryGetValue(name, out string? value) || string.IsNullOrWhiteSpace(value))
        {
            throw new ArgumentException($"Required argument '{name}' is missing.");
        }
        return value;
    }
}
