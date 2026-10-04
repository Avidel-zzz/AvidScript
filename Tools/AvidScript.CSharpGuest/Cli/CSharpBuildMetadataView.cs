using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;
using AvidScript.CSharpSemantic;

namespace AvidScript.CSharpGuest;

internal static class CSharpBuildMetadataView
{
    private static readonly HashSet<string> Fields = new(StringComparer.Ordinal)
    {
        "schema_version", "semantic_version", "succeeded", "source", "diagnostics", "reachability",
        "symbols", "callables", "ue_type_declarations", "ue_method_catalog", "async_methods",
        "exception_flows", "delegate_event_callbacks", "event_subscriptions",
    };

    // The caller has already admitted these same bytes through the full compiler.
    // This view only avoids materializing unused trees in the PowerShell consumer.
    internal static JsonDocument Create(ReadOnlySpan<byte> bytes)
    {
        using var original = JsonDocument.Parse(bytes.ToArray(), new JsonDocumentOptions
        {
            MaxDepth = SemanticSerializer.MaximumDepth,
        });
        var root = original.RootElement;
        ValidatePropertyNames(root);
        var reachable = new HashSet<string>(StringComparer.Ordinal);
        if (root.TryGetProperty("reachability", out var reachability) && reachability.ValueKind == JsonValueKind.Object
            && reachability.TryGetProperty("reachable_callable_ids", out var ids) && ids.ValueKind == JsonValueKind.Array)
            foreach (var id in ids.EnumerateArray())
                reachable.Add(id.GetString() ?? throw new InvalidDataException("Build metadata callable identity is null."));

        using var output = new MemoryStream();
        using (var writer = new Utf8JsonWriter(output, new JsonWriterOptions { MaxDepth = SemanticSerializer.MaximumDepth }))
        {
            writer.WriteStartObject();
            foreach (var property in root.EnumerateObject())
            {
                if (property.Name == "control_flow_graphs" && property.Value.ValueKind == JsonValueKind.Array)
                {
                    writer.WritePropertyName(property.Name);
                    writer.WriteStartArray();
                    foreach (var graph in property.Value.EnumerateArray())
                        if (reachable.Contains(graph.GetProperty("method_symbol_id").GetString()
                            ?? throw new InvalidDataException("Build metadata graph identity is null.")))
                            graph.WriteTo(writer);
                    writer.WriteEndArray();
                }
                else if (Fields.Contains(property.Name) || property.Name == "control_flow_graphs")
                {
                    property.WriteTo(writer);
                }
            }
            writer.WriteEndObject();
        }
        return JsonDocument.Parse(output.ToArray(), new JsonDocumentOptions { MaxDepth = SemanticSerializer.MaximumDepth });
    }

    private static void ValidatePropertyNames(JsonElement value)
    {
        if (value.ValueKind == JsonValueKind.Object)
        {
            var names = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            foreach (var property in value.EnumerateObject())
            {
                if (property.Name.Length == 0 || names.TryGetValue(property.Name, out var prior) && prior != property.Name)
                    throw new InvalidDataException("Build metadata cannot represent empty or case-conflicting JSON property names.");
                names[property.Name] = property.Name;
                ValidatePropertyNames(property.Value);
            }
        }
        else if (value.ValueKind == JsonValueKind.Array)
        {
            foreach (var item in value.EnumerateArray()) ValidatePropertyNames(item);
        }
    }
}
