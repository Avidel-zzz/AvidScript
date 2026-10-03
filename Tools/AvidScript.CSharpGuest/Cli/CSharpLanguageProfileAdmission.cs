using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text.Json;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

public static class CSharpLanguageProfileAdmission
{
    public static string Describe(string name, ReadOnlySpan<byte> semanticArtifact, string? moduleId = null)
    {
        var profile = CSharpLanguageProfile.Resolve(name);
        string semanticHash = Hash(semanticArtifact);
        var document = SemanticArtifactReader.Deserialize(semanticArtifact);
        var result = CSharpGuestCompiler.Compile(document, semanticHash, boundedLanguageErrors: true,
            requestedModuleId: moduleId);
        if (!result.Succeeded || result.Module is null)
            throw new InvalidDataException("ASCG1031: Profile source admission failed: "
                + string.Join("; ", result.Diagnostics.Select(d => d.Code + ": " + d.Message)));
        var module = result.Module;
        return JsonSerializer.Serialize(new
        {
            schema_version = 1,
            language_profile = profile.Identity,
            semantic_sha256 = semanticHash,
            source_sha256 = document.Source.Sha256,
            frontend_source_sha256 = document.Source.FrontendSha256,
            source_id = document.Source.SourceId,
            semantic_schema_version = document.SchemaVersion,
            semantic_version = document.SemanticVersion,
            semantic_succeeded = document.Succeeded,
            module_id = module.ModuleId,
            guest_schema_version = module.SchemaVersion,
            guest_ir_version = module.IrVersion,
            guest_ir_sha256 = Hash(GuestIrSerializer.Serialize(module)),
            compiler_imports = module.Imports.Where(import => import.DispatchClass == "semantic").ToArray(),
        }, new JsonSerializerOptions { PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower });
    }

    internal static int Run(string[] args)
    {
        if (args.Length == 2 && args[0] == "--describe-language-profile")
        {
            Console.WriteLine(CSharpLanguageProfile.Resolve(args[1]).DescribeJson());
            return 0;
        }
        if (args.Length is not (4 or 6) || args[0] != "--validate-language-profile"
            || args[2] != "--semantic" || args.Length == 6 && args[4] != "--module-id"
            || args.Any(string.IsNullOrWhiteSpace))
            throw new ArgumentException("Usage: --validate-language-profile <name> --semantic <path> [--module-id <id>]");
        Console.WriteLine(Describe(args[1], File.ReadAllBytes(args[3]), args.Length == 6 ? args[5] : null));
        return 0;
    }

    private static string Hash(ReadOnlySpan<byte> bytes) => Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
}
