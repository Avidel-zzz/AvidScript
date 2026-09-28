using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using AvidScript.GuestIr;

internal static class GuestCancellationTokenTests
{
    public static int Run()
    {
        int count = 0;
        void Check(bool valid, string message) { if (!valid) throw new InvalidOperationException(message); count++; }
        void Valid(GuestModule module) {
            var result = GuestModuleValidator.Validate(module);
            Check(result.Succeeded, "Token IR: " + string.Join(" | ", result.Diagnostics.Select(item => item.Code + ": " + item.Message)));
        }
        void Reject(GuestModule module, string reason, string code = "ASIR1041") {
            var result = GuestModuleValidator.Validate(module);
            Check(!result.Succeeded && result.Diagnostics.Any(item => item.Code == code), "Token IR did not reject " + reason
                + ": " + string.Join(" | ", result.Diagnostics.Select(item => item.Code + ": " + item.Message)));
        }
        var plain = GuestCancellationTokenFixture.Values();
        var reader = GuestCancellationTokenFixture.WithReader();
        foreach (var module in new[] { plain, reader }) {
            Valid(module);
            byte[] json = GuestIrSerializer.Serialize(module);
            var restored = GuestIrSerializer.Deserialize(json);
            Valid(restored);
            Check(json.SequenceEqual(GuestIrSerializer.Serialize(restored)), "Token IR canonical round trip");
            Check(Encoding.UTF8.GetString(json).Contains("\"cancellation_tokens\"", StringComparison.Ordinal), "Token plan omitted");
        }
        Check(!Encoding.UTF8.GetString(GuestIrSerializer.Serialize(GuestModuleValidationTests.CreateMinimalModule()))
            .Contains("cancellation_tokens", StringComparison.Ordinal), "Old JSON shape changed");
        GuestModule asyncReader = reader with {
            SchemaVersion = GuestComposableCapabilities.SchemaVersion,
            IrVersion = GuestComposableCapabilities.IrVersion,
            Provenance = reader.Provenance with { SemanticSchemaVersion = 54, SemanticVersion = "1.63" },
            CancellationTokens = new(29, "1.28"),
            CapabilityManifest = GuestCapabilityManifest.Create(29, "1.28", new[] {
                new GuestCapability(GuestComposableCapabilities.StaticStorage, 1),
                new GuestCapability(GuestComposableCapabilities.AwaitReadiness, 1),
                new GuestCapability(GuestComposableCapabilities.CancellationIdentity, 1),
                new GuestCapability(GuestComposableCapabilities.ExceptionValues, 1),
                new GuestCapability(GuestComposableCapabilities.CancellationTokenValue, 1),
            }),
        };
        var pendingAsync = GuestModuleValidator.Validate(asyncReader);
        Check(!pendingAsync.Succeeded && pendingAsync.Diagnostics.All(item => item.Code != "ASIR1041"),
            "IR 35 base 29 recognizes a canonical exception token reader but still rejects the incomplete async module");
        Reject(asyncReader with { CapabilityManifest = asyncReader.CapabilityManifest! with {
            Capabilities = asyncReader.CapabilityManifest.Capabilities.Where(capability =>
                capability.Id != GuestComposableCapabilities.ExceptionValues).ToArray() } },
            "async token reader without its exception-value capability");
        Reject(asyncReader with { Provenance = reader.Provenance },
            "async token reader with old Semantic provenance");
        Reject(asyncReader with { CancellationTokens = new(17, "1.16") },
            "async token reader with a synchronous token plan");
        Reject(reader with { CancellationTokens = new(29, "1.28"),
            TaskErrorTransfers = new(29, "1.28", Array.Empty<GuestTaskErrorTransferSite>()) },
            "legacy IR 34 cannot gain task transfers through an IR 35 reader rule");
        foreach (var (schema, version) in new[] { (14, "1.13"), (17, "1.16"), (29, "1.28"), (33, "1.32"), (35, "1.34") })
            Reject(plain with { SchemaVersion = schema, IrVersion = version }, "outer version " + schema);
        Reject(plain with { IrVersion = "1.34" }, "mismatched outer version");
        Reject(plain with { Language = "rust" }, "wrong frontend");
        Reject(plain with { CancellationTokens = null }, "missing token plan");
        Reject(plain with { CancellationTokens = new(14, "1.14") }, "mismatched base pair");
        Reject(plain with { CancellationTokens = new(18, "1.17") }, "unsupported base");
        Reject(plain with { CancellationTokens = new(14, null!) }, "null base version", "ASIR1001");
        Reject(plain with { Provenance = plain.Provenance with { SemanticSchemaVersion = 52 } }, "wrong source schema");
        Reject(plain with { Provenance = plain.Provenance with { SemanticVersion = "1.61" } }, "wrong source version");
        var token = GuestCancellationTokens.ValueType();
        foreach (var invalid in new[] {
            token with { Kind = "scalar", Storage = "i64" }, token with { Size = 4 }, token with { Alignment = 4 },
            token with { ElementTypeId = "type:int64" }, token with { UnderlyingTypeId = "type:int64" },
            token with { Fields = Array.Empty<GuestField>() },
            token with { Fields = new[] { token.Fields[0] with { Offset = 4 } } },
            token with { Fields = new[] { token.Fields[0] with { TypeId = "type:int32" } } },
            token with { Fields = new[] { token.Fields[0] with { Name = "Value" } } },
            token with { Fields = new[] { token.Fields[0] with { Id = "field:$cancellation_token:future" } } },
        }) Reject(plain with { Types = plain.Types.Select(type => type.Id == token.Id ? invalid : type).ToArray() }, "token layout");
        Reject(plain with { Types = plain.Types.Where(type => type.Id != token.Id).ToArray() }, "missing nominal type");
        Reject(plain with { Types = plain.Types.Append(token with { Id = "type:forged" }).ToArray() }, "reserved field owner");
        Reject(plain with { Types = plain.Types.Select(type => type.Id == "type:int64" ? type with {
            Kind = "handle" } : type).ToArray() }, "untyped handle substituted for identity");
        Reject(plain with { Imports = new[] { GuestCancellationTokens.Reader() } }, "reader in plain values");
        Reject(plain with { LanguageErrorCatalog = reader.LanguageErrorCatalog }, "catalog on plain values");
        Reject(plain with { LanguageOutcomeTypes = reader.LanguageOutcomeTypes }, "outcomes on plain values");
        Reject(plain with { ExceptionValues = new(Array.Empty<GuestExceptionValueBinding>()) }, "async bindings on plain values");
        foreach (var invalid in new[] {
            GuestCancellationTokens.Reader() with { Id = "import:alias" },
            GuestCancellationTokens.Reader() with { Name = "avid_exception_cancellation_token_v2" },
            GuestCancellationTokens.Reader() with { Module = "env" },
            GuestCancellationTokens.Reader() with { ReturnTypeId = "type:int32" },
            GuestCancellationTokens.Reader() with { ParameterTypeIds = new[] { "type:int64" } },
            GuestCancellationTokens.Reader() with { DispatchClass = "binding" },
            GuestCancellationTokens.Reader() with { OptimizationClass = "pure" },
            GuestCancellationTokens.Reader() with { BindingOrdinal = 0 },
        }) Reject(reader with { Imports = new[] { reader.Imports[0], invalid } }, "reader signature or alias");
        Reject(reader with { Imports = reader.Imports.Append(GuestCancellationTokens.Reader() with { Id = "import:duplicate" }).ToArray() }, "duplicate reader");
        Reject(reader with { Types = reader.Types.Select(type => type.Id == GuestCancellationTokens.RootTypeId ? type with {
            Kind = "scalar", ElementTypeId = null } : type).ToArray() }, "untraced root substituted for exception");
        Reject(reader with { Exports = reader.Exports.Append(new GuestExport("read", "read")).ToArray() }, "raw managed-reference export", "ASIR1013");
        Reject(reader with { SchemaVersion = 17, IrVersion = "1.16", CancellationTokens = null,
            Provenance = reader.Provenance with { SemanticSchemaVersion = 34, SemanticVersion = "1.43" },
            Types = reader.Types.Where(type => type.Id != GuestCancellationTokens.TypeId).ToArray(),
        }, "reader smuggled into old contract without token metadata");
        Reject(reader with { LanguageErrorCatalog = null }, "missing error catalog", "ASIR1027");
        Reject(reader with { LanguageOutcomeTypes = null }, "missing outcomes", "ASIR1024");
        Reject(reader with { LanguageErrorCatalog = reader.LanguageErrorCatalog! with {
            Sources = new[] { reader.LanguageErrorCatalog!.Sources[0] with { Start = 32 } } } }, "bad source bounds", "ASIR1027");
        foreach (string member in new[] { "base_schema_version", "base_ir_version", "future_field" }) {
            var json = JsonNode.Parse(GuestIrSerializer.Serialize(plain))!.AsObject();
            var plan = json["cancellation_tokens"]!.AsObject();
            if (member == "future_field") plan[member] = 1; else plan.Remove(member);
            bool rejected = false;
            try { GuestIrSerializer.Deserialize(JsonSerializer.SerializeToUtf8Bytes(json)); }
            catch (InvalidDataException) { rejected = true; }
            Check(rejected, "Token JSON must reject " + member);
        }
        return count;
    }
}
