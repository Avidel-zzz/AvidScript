using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AvidScript.UeTypeGenerator;

public sealed record UeTypeGenerationManifest(
    [property: JsonPropertyOrder(0)] int SchemaVersion,
    [property: JsonPropertyOrder(1)] string GeneratorVersion,
    [property: JsonPropertyOrder(2)] int SemanticSchemaVersion,
    [property: JsonPropertyOrder(3)] string SemanticVersion,
    [property: JsonPropertyOrder(4)] string SemanticArtifactSha256,
    [property: JsonPropertyOrder(5)] string GenerationKeySha256,
    [property: JsonPropertyOrder(6)] string ModuleName,
    [property: JsonPropertyOrder(7)] string UnrealVersion,
    [property: JsonPropertyOrder(8)] IReadOnlyList<UeTypeManifestEntry> Types,
    [property: JsonPropertyOrder(9)] IReadOnlyList<UeGeneratedFileEntry> Outputs);

public sealed record UeTypeManifestEntry(
    [property: JsonPropertyOrder(0)] int TypeOrdinal,
    [property: JsonPropertyOrder(1)] string StableTypeId,
    [property: JsonPropertyOrder(2)] string StableSymbolId,
    [property: JsonPropertyOrder(3)] string EngineName,
    [property: JsonPropertyOrder(4)] string CppName,
    [property: JsonPropertyOrder(5)] string Kind,
    [property: JsonPropertyOrder(6)] string BaseCppName,
    [property: JsonPropertyOrder(7)] IReadOnlyList<string> Flags,
    [property: JsonPropertyOrder(8)] IReadOnlyList<UePropertyManifestEntry> Properties,
    [property: JsonPropertyOrder(9)] IReadOnlyList<UeFunctionManifestEntry> Functions);

public sealed record UePropertyManifestEntry(
    [property: JsonPropertyOrder(0)] int MemberOrdinal,
    [property: JsonPropertyOrder(1)] string StableMemberId,
    [property: JsonPropertyOrder(2)] string Name,
    [property: JsonPropertyOrder(3)] string CppType,
    [property: JsonPropertyOrder(4)] IReadOnlyList<string> Flags,
    [property: JsonPropertyOrder(5)] string Category,
    [property: JsonPropertyOrder(6)] string ReplicatedUsing,
    [property: JsonPropertyOrder(7)] string GetterImportName,
    [property: JsonPropertyOrder(8)] string SetterImportName);

public sealed record UeFunctionManifestEntry(
    [property: JsonPropertyOrder(0)] int MemberOrdinal,
    [property: JsonPropertyOrder(1)] string StableMemberId,
    [property: JsonPropertyOrder(2)] string Name,
    [property: JsonPropertyOrder(3)] string NativeName,
    [property: JsonPropertyOrder(4)] string ExportName,
    [property: JsonPropertyOrder(5)] string ReturnCppType,
    [property: JsonPropertyOrder(6)] IReadOnlyList<UeFunctionParameterEntry> Parameters,
    [property: JsonPropertyOrder(7)] IReadOnlyList<string> Flags,
    [property: JsonPropertyOrder(8)] string Category,
    [property: JsonPropertyOrder(9)] string Accessibility);

public sealed record UeFunctionParameterEntry(
    [property: JsonPropertyOrder(0)] int Ordinal,
    [property: JsonPropertyOrder(1)] string Name,
    [property: JsonPropertyOrder(2)] string CppType,
    [property: JsonPropertyOrder(3)] string RefKind);

public sealed record UeGeneratedFileEntry(
    [property: JsonPropertyOrder(0)] string RelativePath,
    [property: JsonPropertyOrder(1)] string Sha256,
    [property: JsonPropertyOrder(2)] int Length);

public sealed record UeTypeGenerationResult(
    UeTypeGenerationManifest Manifest,
    IReadOnlyDictionary<string, byte[]> Files);
