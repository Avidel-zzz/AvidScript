using System;
using System.Collections.Generic;
using System.Linq;
using System.Security.Cryptography;
using AvidScript.GuestIr;

internal static class GuestTaskCancellationErrorTests
{
    public static int Run()
    {
        int count = 0;
        GuestModule module = GuestTaskCancellationFixture.Create();
        Valid(module);
        count++;
        byte[] canonical = GuestIrSerializer.Serialize(module);
        GuestModule restored = GuestIrSerializer.Deserialize(canonical);
        Valid(restored);
        Check(canonical.SequenceEqual(GuestIrSerializer.Serialize(restored)), "IR 24 canonical round trip");
        count++;
        GuestModule operation = WithType(module, GuestTaskCancellationFixture.OperationCanceledType);
        Valid(operation);
        Check(!SHA256.HashData(canonical).SequenceEqual(SHA256.HashData(GuestIrSerializer.Serialize(operation))),
            "cancellation type changes artifact identity");
        count++;

        void Reject(GuestModule candidate, string code)
        {
            GuestValidationResult result = GuestModuleValidator.Validate(candidate);
            Check(!result.Succeeded && result.Diagnostics.Any(item => item.Code == code),
                $"expected {code}: " + string.Join(" | ", result.Diagnostics.Select(item => item.Message)));
            count++;
        }
        foreach (var (schema, version) in new[]
        {
            (17, "1.16"), (18, "1.17"), (19, "1.18"), (20, "1.19"),
            (21, "1.20"), (22, "1.21"), (23, "1.22"), (25, "1.24"),
        }) Reject(module with { SchemaVersion = schema, IrVersion = version }, "ASIR1032");
        Reject(module with { IrVersion = "1.22" }, "ASIR1032");
        Reject(module with { Language = "other" }, "ASIR1032");
        Reject(module with { Provenance = module.Provenance with { SemanticSchemaVersion = 43 } }, "ASIR1032");
        Reject(module with { Provenance = module.Provenance with { SemanticVersion = "1.52" } }, "ASIR1032");
        Reject(module with { LanguageErrorCatalog = null }, "ASIR1032");
        Reject(module with { LanguageErrorCatalog = new(Array.Empty<GuestLanguageErrorTypeToken>(),
            Array.Empty<GuestLanguageErrorSourceToken>()) }, "ASIR1032");
        Reject(WithType(module, "type:global::System.Exception"), "ASIR1032");
        Reject(WithType(module, "type:global::User.TaskCanceledException"), "ASIR1032");
        Reject(module with { LanguageErrorCatalog = module.LanguageErrorCatalog! with
        {
            Sources = new[] { module.LanguageErrorCatalog!.Sources[0] with { Start = 31 } },
        } }, "ASIR1027");
        Reject(module with { AsyncExceptionRoutes = Array.Empty<GuestAsyncExceptionRoute>() }, "ASIR1030");
        Reject(module with { DirectAwaitRoutes = Array.Empty<GuestDirectAwaitRoute>() }, "ASIR1031");
        Reject(module with { Imports = module.Imports.Append(new GuestImport(
            "import:unowned_root", "avidscript", "unowned_root", Array.Empty<string>(),
            GuestTaskCancellationFixture.Root)).ToArray() }, "ASIR1013");
        Reject(module with { LanguageErrorCatalog = module.LanguageErrorCatalog! with
        {
            Types = new[]
            {
                new GuestLanguageErrorTypeToken(1, GuestTaskCancellationFixture.OperationCanceledType),
                new GuestLanguageErrorTypeToken(2, GuestTaskCancellationFixture.TaskCanceledType),
            },
        } }, "ASIR1027");

        string[] imports = { GuestTaskCancellationErrorValidator.ImportId,
            GuestTaskCancellationErrorValidator.MetaImportId, GuestTaskCancellationErrorValidator.RootImportId };
        foreach (string id in imports)
        {
            Reject(module with { Imports = module.Imports.Where(item => item.Id != id).ToArray() }, "ASIR1032");
            foreach (Func<GuestImport, GuestImport> mutation in new Func<GuestImport, GuestImport>[]
            {
                item => item with { Module = "env" },
                item => item with { Id = item.Id + ":alias" },
                item => item with { Name = item.Name + "_alias" },
                item => item with { DispatchClass = "direct" },
                item => item with { OptimizationClass = "pure" },
                item => item with { BindingOrdinal = 0 },
                item => item with { ParameterTypeIds = new[] { "type:int32" } },
                item => item with { ReturnTypeId = "type:void" },
            }) Reject(module with { Imports = module.Imports.Select(item => item.Id == id ? mutation(item) : item).ToArray() }, "ASIR1032");
            GuestImport duplicate = module.Imports.Single(item => item.Id == id) with { Id = id + ":duplicate" };
            Reject(module with { Imports = module.Imports.Append(duplicate).ToArray() }, "ASIR1032");
            // Checking by both name and id also catches imports hidden in another namespace/version.
            Reject(module with { SchemaVersion = 14, IrVersion = "1.13", Imports = new[]
                { module.Imports.Single(item => item.Id == id) with { Module = "env" } } }, "ASIR1032");
        }
        foreach (string id in new[] { "import:task", "import:bind", "import:propagate", "import:retain" })
            Reject(module with { Imports = module.Imports.Where(item => item.Id != id).ToArray() }, "ASIR1029");
        Reject(module with { Imports = module.Imports.Where(item => item.Id is not
            ("import:task" or "import:bind" or "import:propagate" or "import:retain")).ToArray() }, "ASIR1028");
        Reject(module with { Imports = module.Imports.Where(item => item.Id != GuestTaskLanguageErrorValidator.ImportId).ToArray() }, "ASIR1029");
        Reject(module with { Types = module.Types.Select(type => type.Id == GuestTaskCancellationFixture.Root
            ? type with { ElementTypeId = null } : type).ToArray() }, "ASIR1029");
        Reject(ChangeProducer(module, instructions => instructions.Where(item => item.Op != "managed_new").ToArray()), "ASIR1029");
        Reject(ChangeProducer(module, instructions => instructions.Where(item => item.Op != "managed_set").ToArray()), "ASIR1029");
        Reject(ChangeProducer(module, instructions => instructions.Select(item => item.Op == "managed_set"
            ? item with { OperandIds = new[] { "root", "source" } } : item).ToArray()), "ASIR1029");
        foreach (string value in new[] { "0", "2", "01", "-1" })
            Reject(ChangeProducer(module, instructions => instructions.Select(item => item.ResultId == "type"
                ? item with { Constant = new("int32", value) } : item).ToArray()), "ASIR1029");
        Reject(ChangeProducer(module, instructions => instructions.Select(item => item.ResultId == "source"
            ? item with { Constant = new("int32", "2") } : item).ToArray()), "ASIR1029");
        Reject(ChangeProducer(module, instructions => instructions.Append(new GuestInstruction("local_store", null,
            new[] { "source" }, "type", null, null)).ToArray()), "ASIR1029");
        Reject(ChangeProducer(module, instructions => instructions.Skip(1).Append(instructions[0]).ToArray()), "ASIR1032");

        GuestImport metadata = new(GuestTaskLanguageErrorValidator.MetaImportId, "avidscript",
            GuestTaskLanguageErrorValidator.MetaImportName, new[] { "type:int64" }, "type:int64");
        GuestImport root = new(GuestTaskLanguageErrorValidator.RootImportId, "avidscript",
            GuestTaskLanguageErrorValidator.RootImportName, new[] { "type:int64" }, GuestTaskCancellationFixture.Root);
        Valid(module with { Imports = module.Imports.Concat(new[] { metadata, root }).ToArray() });
        count++;
        Reject(module with { Imports = module.Imports.Append(metadata).ToArray() }, "ASIR1029");
        Reject(module with { Imports = module.Imports.Append(root).ToArray() }, "ASIR1029");
        Reject(module with { LanguageErrorCatalog = module.LanguageErrorCatalog! with
        {
            Sources = new GuestLanguageErrorSourceToken[] { null! },
        } }, "ASIR1001");
        return count;
    }

    private static GuestModule WithType(GuestModule module, string type) => module with
    {
        LanguageErrorCatalog = module.LanguageErrorCatalog! with
        {
            Types = new[] { new GuestLanguageErrorTypeToken(1, type) },
        },
    };

    private static GuestModule ChangeProducer(GuestModule module,
        Func<IReadOnlyList<GuestInstruction>, IReadOnlyList<GuestInstruction>> change) => module with
    {
        Functions = module.Functions.Select(function => function.Id == "cancel" ? function with
        {
            Blocks = new[] { function.Blocks[0] with { Instructions = change(function.Blocks[0].Instructions) } },
        } : function).ToArray(),
    };

    private static void Valid(GuestModule module)
    {
        GuestValidationResult result = GuestModuleValidator.Validate(module);
        Check(result.Succeeded, string.Join(" | ", result.Diagnostics.Select(item => item.Message)));
    }

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}
