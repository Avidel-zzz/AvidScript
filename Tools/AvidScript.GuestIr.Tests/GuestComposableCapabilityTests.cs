using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using AvidScript.GuestIr;

internal static class GuestComposableCapabilityTests
{
    public static int Run()
    {
        int count = 0;
        void Check(bool condition, string message)
        {
            if (!condition) throw new InvalidOperationException(message);
            count++;
        }
        void Reject(GuestModule module, string reason, string code = "ASIR1042")
        {
            var result = GuestModuleValidator.Validate(module);
            Check(!result.Succeeded && result.Diagnostics.Any(item => item.Code == code),
                reason + ": " + string.Join(" | ", result.Diagnostics.Select(item => item.Code + ": " + item.Message)));
        }

        GuestModule old = GuestModuleValidationTests.CreateMinimalModule();
        byte[] oldBytes = GuestIrSerializer.Serialize(old);
        Check(!Encoding.UTF8.GetString(oldBytes).Contains("capability_manifest", StringComparison.Ordinal),
            "Legacy IR must omit the new optional field.");
        Check(oldBytes.SequenceEqual(GuestIrSerializer.Serialize(GuestIrSerializer.Deserialize(oldBytes))),
            "Legacy IR bytes must remain canonical.");
        Check(GuestModuleValidator.Validate(old).Succeeded, "Legacy IR still validates.");

        GuestModule candidate = old with
        {
            SchemaVersion = GuestComposableCapabilities.SchemaVersion,
            IrVersion = GuestComposableCapabilities.IrVersion,
            Provenance = old.Provenance with { SemanticSchemaVersion = 54, SemanticVersion = "1.63" },
            CapabilityManifest = new(14, "1.13", Array.Empty<GuestCapability>()),
        };
        var pending = GuestModuleValidator.Validate(candidate);
        Check(!pending.Succeeded && pending.Diagnostics.Any(item => item.Code == "ASIR1042"),
            "An empty IR 35 capability set remains outside the admitted synchronous composition.");
        byte[] bytes = GuestIrSerializer.Serialize(candidate);
        Check(Encoding.UTF8.GetString(bytes).Contains("\"capability_manifest\"", StringComparison.Ordinal),
            "The new manifest must appear only in IR 35 artifacts.");
        Check(bytes.SequenceEqual(GuestIrSerializer.Serialize(GuestIrSerializer.Deserialize(bytes))),
            "A well-formed manifest has a canonical JSON round trip.");
        GuestCapability[] unordered = {
            new(GuestComposableCapabilities.StaticStorage, 1),
            new(GuestComposableCapabilities.AwaitReadiness, 1),
        };
        GuestCapabilityManifest ascending = GuestCapabilityManifest.Create(29, "1.28", unordered);
        GuestCapabilityManifest descending = GuestCapabilityManifest.Create(29, "1.28", unordered.Reverse());
        Check(GuestIrSerializer.Serialize(candidate with { CapabilityManifest = ascending })
            .SequenceEqual(GuestIrSerializer.Serialize(candidate with { CapabilityManifest = descending })),
            "Capability construction sorts equivalent input sets to the same canonical bytes.");

        GuestModule composed = GuestComposableCapabilityFixture.SynchronousStaticToken();
        GuestValidationResult composition = GuestModuleValidator.Validate(composed);
        Check(composition.Succeeded,
            "A real IR 35 module validates static and token instructions together: "
                + string.Join(" | ", composition.Diagnostics.Select(item => item.Code + ": " + item.Message)));
        byte[] composedBytes = GuestIrSerializer.Serialize(composed);
        Check(composedBytes.SequenceEqual(GuestIrSerializer.Serialize(GuestIrSerializer.Deserialize(composedBytes))),
            "The admitted composition has canonical bytes.");
        Check(GuestModuleValidator.Validate(GuestIrSerializer.Deserialize(composedBytes)).Succeeded,
            "Canonical IR 35 bytes validate on the real module after deserialization.");
        Reject(composed with { Language = "unregistered-language" },
            "An unknown frontend cannot publish IR 35 under the C# contract");
        Reject(composed with { CancellationTokens = null }, "missing token plan in a real composite module");
        Reject(composed with { StaticStorage = null }, "missing static plan in a real composite module");
        Reject(composed with { DirectAwaitRoutes = Array.Empty<GuestDirectAwaitRoute>() },
            "unlisted async route metadata in a synchronous composite module");
        Reject(composed with { LanguageOutcomeTypes = Array.Empty<GuestLanguageOutcomeType>() },
            "unlisted language-error metadata in a synchronous composite module");
        Reject(composed with { CapabilityManifest = composed.CapabilityManifest! with { ExecutionBaseSchemaVersion = 17 } },
            "real composite module with conflicting execution base");
        Reject(composed with { Types = composed.Types.Where(type => type.Id != GuestCancellationTokens.TypeId).ToArray() },
            "real composite module without its token value type", "ASIR1041");
        Reject(composed with { Functions = composed.Functions.Select(function => function.Id == "read" ? function with
        {
            Blocks = function.Blocks.Select(block => block with
            {
                Instructions = block.Instructions.Select(instruction => instruction.Op == GuestStaticStorage.GetOp
                    ? instruction with { TargetId = "static:missing" } : instruction).ToArray(),
            }).ToArray(),
        } : function).ToArray() }, "real composite module with an invalid static access", "ASIR1035");
        Reject(composed with { Imports = composed.Imports.Append(GuestCancellationTokens.Reader()).ToArray() },
            "exception-token reader in a synchronous composition", "ASIR1041");

        Reject(old with { CapabilityManifest = candidate.CapabilityManifest }, "old IR with new metadata");
        Reject(candidate with { SchemaVersion = 36 }, "future IR with this manifest");
        Reject(candidate with { CapabilityManifest = null }, "missing manifest");
        Reject(candidate with { CapabilityManifest = new(14, "1.16", Array.Empty<GuestCapability>()) },
            "mismatched base pair");
        Reject(candidate with { CapabilityManifest = new(30, "1.29", Array.Empty<GuestCapability>()) },
            "unsupported base");
        Reject(candidate with { Provenance = old.Provenance }, "old C# Semantic version");
        Reject(candidate with { CapabilityManifest = new(14, "1.13", new[] {
            new GuestCapability("future.capability", 1),
        }) }, "unknown capability");
        Reject(candidate with { CapabilityManifest = new(14, "1.13", new[] {
            new GuestCapability(GuestComposableCapabilities.StaticStorage, 2),
        }) }, "future capability version");
        Reject(candidate with { CapabilityManifest = new(14, "1.13", new[] {
            new GuestCapability(GuestComposableCapabilities.StaticStorage, 1),
            new GuestCapability(GuestComposableCapabilities.AwaitReadiness, 1),
        }) }, "unsorted capability list");
        Reject(candidate with { CapabilityManifest = new(14, "1.13", new[] {
            new GuestCapability(GuestComposableCapabilities.StaticStorage, 1),
            new GuestCapability(GuestComposableCapabilities.StaticStorage, 1),
        }) }, "duplicate capability");
        Reject(candidate with { CapabilityManifest = new(14, "1.13", new[] {
            new GuestCapability(GuestComposableCapabilities.StaticStorage, 1),
        }) }, "declared capability without a plan");
        Reject(candidate with { StaticStorage = new(14, "1.13", Array.Empty<GuestStaticSlot>()) },
            "plan without a declared capability");
        Reject(candidate with {
            CapabilityManifest = new(14, "1.13", new[] {
                new GuestCapability(GuestComposableCapabilities.StaticStorage, 1),
            }),
            StaticStorage = new(17, "1.16", Array.Empty<GuestStaticSlot>()),
        }, "conflicting plan base");
        Reject(candidate with {
            CapabilityManifest = new(14, "1.13", new[] {
                new GuestCapability(GuestComposableCapabilities.CancellationIdentity, 1),
            }),
            CancellationIdentity = new(14, "1.13"),
        }, "async capability on synchronous base");

        foreach (string field in new[] {
            "execution_base_schema_version", "execution_base_ir_version", "capabilities",
        })
        {
            JsonObject json = JsonNode.Parse(bytes)!.AsObject();
            json["capability_manifest"]!.AsObject().Remove(field);
            bool failed = false;
            try { GuestIrSerializer.Deserialize(JsonSerializer.SerializeToUtf8Bytes(json)); }
            catch (InvalidDataException) { failed = true; }
            Check(failed, "Missing manifest field must fail JSON reading: " + field);
        }
        JsonObject unknown = JsonNode.Parse(bytes)!.AsObject();
        unknown["capability_manifest"]!.AsObject()["future_field"] = 1;
        bool unknownFailed = false;
        try { GuestIrSerializer.Deserialize(JsonSerializer.SerializeToUtf8Bytes(unknown)); }
        catch (InvalidDataException) { unknownFailed = true; }
        Check(unknownFailed, "Unknown manifest fields must fail JSON reading.");
        JsonObject topLevelUnknown = JsonNode.Parse(bytes)!.AsObject();
        topLevelUnknown["future_field"] = 1;
        bool topLevelFailed = false;
        try { GuestIrSerializer.Deserialize(JsonSerializer.SerializeToUtf8Bytes(topLevelUnknown)); }
        catch (InvalidDataException) { topLevelFailed = true; }
        Check(topLevelFailed, "Unknown IR 35 top-level fields must fail JSON reading.");
        foreach (string duplicate in new[] { "\"schema_version\": 35,", "\"execution_base_schema_version\": 14," })
        {
            string json = Encoding.UTF8.GetString(bytes).Replace(
                duplicate, duplicate + "\n" + duplicate, StringComparison.Ordinal);
            Check(!string.Equals(json, Encoding.UTF8.GetString(bytes), StringComparison.Ordinal),
                "Duplicate-key fixture must change JSON.");
            bool failed = false;
            try { GuestIrSerializer.Deserialize(Encoding.UTF8.GetBytes(json)); }
            catch (InvalidDataException) { failed = true; }
            Check(failed, "Duplicate IR 35 JSON property must be rejected.");
        }
        Reject(candidate with { CapabilityManifest = new(14, "1.13", null!) },
            "null capability list", "ASIR1001");

        return count;
    }
}
