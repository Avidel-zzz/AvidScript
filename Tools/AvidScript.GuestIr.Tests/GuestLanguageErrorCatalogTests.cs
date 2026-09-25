using System;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using AvidScript.GuestIr;

internal static class GuestLanguageErrorCatalogTests
{
    public static int Run()
    {
        CatalogRoundTripsCanonically();
        CatalogChangesArtifactIdentity();
        OlderVersionRejectsCatalog();
        MismatchedVersionIsRejected();
        Version17RequiresCatalog();
        Version17RequiresOutcomeTypes();
        InvalidSourceSpanIsRejected();
        DuplicateTypeIsRejected();
        UnknownProducerTokenIsRejected();
        UnusedCatalogTokenIsRejected();
        NullEntryFailsClosed();
        OutcomeFlowChecksRemainActive();
        CombinedTaskLanguageErrorVersion();
        return 13;
    }

    private static void CatalogChangesArtifactIdentity()
    {
        GuestModule module = CreateModule();
        GuestLanguageErrorSourceToken source = module.LanguageErrorCatalog!.Sources[0];
        GuestModule changed = module with { LanguageErrorCatalog = module.LanguageErrorCatalog with
        {
            Sources = new[] { source with { Start = 1, Column = 1, EndColumn = 6 } },
        } };
        AssertValid(changed);
        Check(!SHA256.HashData(GuestIrSerializer.Serialize(module))
                .SequenceEqual(SHA256.HashData(GuestIrSerializer.Serialize(changed))),
            "source position changes must change the canonical Guest IR artifact identity");
    }

    private static GuestModule CreateModule()
    {
        GuestModule prior = GuestLanguageOutcomeFlowTests.CreateModule();
        return prior with
        {
            SchemaVersion = GuestLanguageErrorCatalog.SchemaVersion,
            IrVersion = GuestLanguageErrorCatalog.IrVersion,
            LanguageErrorCatalog = new GuestLanguageErrorCatalog(
                new[] { new GuestLanguageErrorTypeToken(1, "type:global::System.Exception") },
                new[] { new GuestLanguageErrorSourceToken(1, "Scripts/Error.cs", 32,
                    0, 5, 0, 0, 0, 5) }),
        };
    }

    private static void CatalogRoundTripsCanonically()
    {
        GuestModule module = CreateModule();
        AssertValid(module);
        byte[] bytes = GuestIrSerializer.Serialize(module);
        Check(Encoding.UTF8.GetString(bytes).Contains("\"language_error_catalog\"", StringComparison.Ordinal)
            && bytes.SequenceEqual(GuestIrSerializer.Serialize(GuestIrSerializer.Deserialize(bytes))),
            "IR 17 must serialize its error catalog canonically");
        GuestModule old = GuestLanguageOutcomeFlowTests.CreateModule();
        Check(!Encoding.UTF8.GetString(GuestIrSerializer.Serialize(old))
                .Contains("language_error_catalog", StringComparison.Ordinal),
            "IR 16 serialization must retain its original shape");
    }

    private static void OlderVersionRejectsCatalog() =>
        AssertError(CreateModule() with { SchemaVersion = 16, IrVersion = "1.15" }, "ASIR1027");

    private static void MismatchedVersionIsRejected() =>
        AssertError(CreateModule() with { IrVersion = "1.17" }, "ASIR1001");

    private static void Version17RequiresCatalog() =>
        AssertError(CreateModule() with { LanguageErrorCatalog = null }, "ASIR1027");

    private static void Version17RequiresOutcomeTypes() =>
        AssertError(CreateModule() with { LanguageOutcomeTypes = null }, "ASIR1024");

    private static void InvalidSourceSpanIsRejected()
    {
        GuestModule module = CreateModule();
        GuestLanguageErrorSourceToken source = module.LanguageErrorCatalog!.Sources[0];
        AssertError(module with { LanguageErrorCatalog = module.LanguageErrorCatalog with
        {
            Sources = new[] { source with { Start = 31, Length = 5 } },
        } }, "ASIR1027");
    }

    private static void DuplicateTypeIsRejected()
    {
        GuestModule module = CreateModule();
        GuestLanguageErrorTypeToken type = module.LanguageErrorCatalog!.Types[0];
        AssertError(module with { LanguageErrorCatalog = module.LanguageErrorCatalog with
        {
            Types = new[] { type, type with { Token = 2 } },
        } }, "ASIR1027");
    }

    private static void UnknownProducerTokenIsRejected()
    {
        GuestModule module = CreateModule();
        GuestFunction leaf = module.Functions.Single(function =>
            function.Id == "function:outcome_leaf");
        AssertError(module with { Functions = module.Functions.Select(function =>
            function.Id == leaf.Id ? leaf with
            {
                Locals = leaf.Locals.Append(new GuestRegister("bad_type", "type:int32")).ToArray(),
                Blocks = leaf.Blocks.Select(block => block.Id switch
                {
                    "entry" => block with { Instructions = block.Instructions.Append(
                        new GuestInstruction("constant", "bad_type", Array.Empty<string>(),
                            null, null, new GuestConstant("int32", "2"))).ToArray() },
                    "error" => block with { Instructions = block.Instructions.Select(instruction =>
                        instruction.Op == "field_store" && instruction.TargetId == "field:error_type"
                            ? instruction with { OperandIds = new[] { "out", "bad_type" } }
                            : instruction).ToArray() },
                    _ => block,
                }).ToArray(),
            } : function).ToArray() }, "ASIR1027");
    }

    private static void UnusedCatalogTokenIsRejected()
    {
        GuestModule module = CreateModule();
        AssertError(module with { LanguageErrorCatalog = module.LanguageErrorCatalog! with
        {
            Types = new[]
            {
                new GuestLanguageErrorTypeToken(1, "type:global::System.Exception"),
                new GuestLanguageErrorTypeToken(2, "type:global::System.InvalidOperationException"),
            },
        } }, "ASIR1027");
    }

    private static void NullEntryFailsClosed()
    {
        GuestModule module = CreateModule();
        AssertError(module with { LanguageErrorCatalog = module.LanguageErrorCatalog! with
        {
            Sources = new GuestLanguageErrorSourceToken[] { null! },
        } }, "ASIR1001");
    }

    private static void OutcomeFlowChecksRemainActive()
    {
        GuestModule module = CreateModule();
        GuestFunction caller = module.Functions.Single(function =>
            function.Id == "function:outcome_caller");
        GuestBasicBlock entry = caller.Blocks.Single(block => block.Id == "entry");
        AssertError(module with { Functions = module.Functions.Select(function =>
            function.Id == caller.Id ? caller with
            {
                Blocks = caller.Blocks.Select(block => block.Id == entry.Id
                    ? entry with { Terminator = new GuestTerminator("branch", null, "success", null, null) }
                    : block).ToArray(),
            } : function).ToArray() }, "ASIR1025");
    }

    private static void CombinedTaskLanguageErrorVersion()
    {
        GuestModule prior = CreateModule();
        GuestImport task = new("import:task_i32_v1", "avidscript", "avid_task_i32_v1",
            new[] { "type:int32", "type:int64", "type:int32", "type:int32" }, "type:int64");
        GuestImport bind = new("import:task_bind_producer_v1", "avidscript",
            "avid_task_bind_producer_v1", new[] { "type:int64", "type:int64" }, "type:int32");
        GuestImport propagate = new("import:task_propagate_failure_v1", "avidscript",
            "avid_task_propagate_failure_v1", new[] { "type:int64", "type:int64" }, "type:int32");
        GuestImport retain = new("import:task_retain_for_continuation_v1", "avidscript",
            "avid_task_retain_for_continuation_v1", new[] { "type:int64", "type:int64" }, "type:int32");
        GuestImport fault = new("import:task_fault_language_error_v1", "avidscript",
            "avid_task_fault_language_error_v1",
            new[] { "type:int64", "type:int32", "type:int32", "type:language_error_root" },
            "type:int32");
        GuestFunction caller = new("function:task_fault_language_error",
            new[]
            {
                new GuestRegister("task", "type:int64"),
                new GuestRegister("root", "type:language_error_root"),
            },
            new[]
            {
                new GuestRegister("type_token", "type:int32"),
                new GuestRegister("source_token", "type:int32"),
                new GuestRegister("accepted", "type:int32"),
            }, "type:int32", "entry", new[]
            {
                new GuestBasicBlock("entry", new GuestInstruction[]
                {
                    new("constant", "type_token", Array.Empty<string>(), null, null,
                        new GuestConstant("int32", "1")),
                    new("constant", "source_token", Array.Empty<string>(), null, null,
                        new GuestConstant("int32", "1")),
                    new("call", "accepted", new[] { "task", "type_token", "source_token", "root" },
                        fault.Id, null, null),
                }, new GuestTerminator("return", null, null, null, "accepted")),
            });
        GuestModule combined = prior with
        {
            SchemaVersion = 20,
            IrVersion = "1.19",
            Provenance = prior.Provenance with
            {
                SemanticSchemaVersion = 40,
                SemanticVersion = "1.49",
            },
            Types = prior.Types.Append(new GuestType("type:int64", "scalar", "i64",
                Array.Empty<GuestField>(), null, null, 8, 8)).ToArray(),
            Imports = prior.Imports.Concat(new[] { task, bind, propagate, retain, fault }).ToArray(),
            Functions = prior.Functions.Append(caller).ToArray(),
        };
        AssertValid(combined);
        byte[] bytes = GuestIrSerializer.Serialize(combined);
        Check(bytes.SequenceEqual(GuestIrSerializer.Serialize(GuestIrSerializer.Deserialize(bytes))),
            "IR 20 task/language-error module must round-trip canonically");
        AssertError(combined with { SchemaVersion = 19, IrVersion = "1.18" }, "ASIR1029");
        AssertError(combined with { Provenance = combined.Provenance with
        {
            SemanticSchemaVersion = 39, SemanticVersion = "1.48",
        } }, "ASIR1029");
        AssertError(combined with { LanguageErrorCatalog = null }, "ASIR1027");
        AssertError(combined with { Imports = combined.Imports.Where(item =>
            item.Name != fault.Name).ToArray() }, "ASIR1029");
        AssertError(combined with { Imports = combined.Imports.Where(item =>
            item.Name != task.Name).ToArray() }, "ASIR1028");
        AssertError(combined with { Imports = combined.Imports.Select(item =>
            item.Name == fault.Name ? item with { ParameterTypeIds = new[]
                { "type:int32", "type:int32", "type:int32", "type:language_error_root" } }
                : item).ToArray() }, "ASIR1029");
        AssertError(combined with { Functions = combined.Functions.Select(function =>
            function.Id == caller.Id ? function with { Blocks = new[]
            {
                caller.Blocks[0] with { Instructions = caller.Blocks[0].Instructions.Select(instruction =>
                    instruction.ResultId == "source_token"
                        ? instruction with { Constant = new GuestConstant("int32", "2") }
                        : instruction).ToArray() },
            } } : function).ToArray() }, "ASIR1029");
    }

    private static void AssertValid(GuestModule module)
    {
        GuestValidationResult result = GuestModuleValidator.Validate(module);
        Check(result.Succeeded, string.Join(" | ", result.Diagnostics.Select(item => item.Message)));
    }

    private static void AssertError(GuestModule module, string code)
    {
        GuestValidationResult result = GuestModuleValidator.Validate(module);
        Check(!result.Succeeded && result.Diagnostics.Any(diagnostic => diagnostic.Code == code),
            $"expected {code}: " + string.Join(" | ", result.Diagnostics.Select(item => item.Message)));
    }

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}
