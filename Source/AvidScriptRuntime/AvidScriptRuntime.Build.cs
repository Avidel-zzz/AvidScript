using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using EpicGames.Core;
using UnrealBuildTool;

public class AvidScriptRuntime : ModuleRules
{
	public AvidScriptRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new[]
			{
				"AvidScriptCore",
				"AvidScriptBindings",
				"AvidScriptVM",
				"Core",
				"CoreUObject",
				"Engine"
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new[]
			{
				"Projects",
				"Json",
				"SSL",
				"TraceLog"
			}
		);

		AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");
		StagePublishedModules(Target);
	}

	private void StagePublishedModules(ReadOnlyTargetRules Target)
	{
		if (Target.ProjectFile == null)
		{
			return;
		}

		string ProjectRoot = Path.GetDirectoryName(Target.ProjectFile.FullName)!;
		string ModulesRoot = Path.GetFullPath(
			Path.Combine(ProjectRoot, "Content", "AvidScript", "Modules"));
		string CatalogPath = Path.Combine(ModulesRoot, "catalog.json");
		if (!File.Exists(CatalogPath))
		{
			if (Target.Type != TargetType.Editor)
			{
				throw new BuildException(
					"AvidScript packaged targets require Content/AvidScript/Modules/catalog.json.");
			}
			return;
		}

		JsonObject Catalog = ReadJson(CatalogPath, "module catalog");
		if (!Catalog.TryGetIntegerField("schema_version", out int SchemaVersion)
			|| SchemaVersion != 1
			|| !Catalog.TryGetObjectArrayField("modules", out JsonObject[] Modules))
		{
			throw new BuildException("AvidScript module catalog schema is invalid.");
		}

		string ExpectedConfiguration =
			Target.Configuration == UnrealTargetConfiguration.Shipping
				? "shipping"
				: "development";
		HashSet<string> StagedFiles = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
		{
			CatalogPath
		};
		foreach (JsonObject Module in Modules)
		{
			if (!Module.TryGetStringField("module_id", out string ModuleId)
				|| !Module.TryGetStringField("package_id", out string PackageId)
				|| !Module.TryGetStringField("descriptor_file", out string DescriptorFile)
				|| !Module.TryGetStringField("platform", out string Platform)
				|| !Module.TryGetStringField("configuration", out string Configuration)
				|| !IsNormalizedModuleId(ModuleId)
				|| !IsLowercaseSha256(PackageId)
				|| Platform != "win64"
				|| (Target.Type != TargetType.Editor
					&& Configuration != ExpectedConfiguration))
			{
				throw new BuildException("AvidScript module catalog entry is invalid for this target.");
			}

			string ExpectedDescriptor = $"{ModuleId}/{PackageId}/package.json";
			if (!String.Equals(DescriptorFile, ExpectedDescriptor, StringComparison.Ordinal))
			{
				throw new BuildException("AvidScript module descriptor path is not canonical.");
			}
			string DescriptorPath = ResolvePackagePath(
				ModulesRoot,
				DescriptorFile,
				"module descriptor");
			JsonObject Descriptor = ReadJson(DescriptorPath, "module descriptor");
			if (!Descriptor.TryGetStringField("module_id", out string DescriptorModuleId)
				|| !Descriptor.TryGetStringField("package_id", out string DescriptorPackageId)
				|| !Descriptor.TryGetStringField("configuration", out string DescriptorConfiguration)
				|| DescriptorModuleId != ModuleId
				|| DescriptorPackageId != PackageId
				|| DescriptorConfiguration != Configuration
				|| !Descriptor.TryGetObjectField("artifacts", out JsonObject Artifacts))
			{
				throw new BuildException("AvidScript module descriptor does not match its catalog entry.");
			}

			string PackageRoot = Path.GetDirectoryName(DescriptorPath)!;
			StagedFiles.Add(DescriptorPath);
			foreach (string ArtifactName in new[]
			{
				"runtime_manifest",
				"canonical_wasm",
				"precompiled",
				"binding_manifest",
				"binding_descriptor"
			})
			{
				StagedFiles.Add(ReadArtifactPath(Artifacts, ArtifactName, PackageRoot));
			}
			if (Artifacts.TryGetObjectField("debug_map", out JsonObject DebugMap))
			{
				StagedFiles.Add(ReadArtifactFilePath(DebugMap, PackageRoot, "debug_map"));
			}
		}

		foreach (string FilePath in StagedFiles)
		{
			ExternalDependencies.Add(FilePath);
			RuntimeDependencies.Add(FilePath, StagedFileType.UFS);
		}
	}

	private static string ReadArtifactPath(
		JsonObject Artifacts,
		string ArtifactName,
		string PackageRoot)
	{
		if (!Artifacts.TryGetObjectField(ArtifactName, out JsonObject Artifact))
		{
			throw new BuildException($"AvidScript package is missing artifact '{ArtifactName}'.");
		}
		return ReadArtifactFilePath(Artifact, PackageRoot, ArtifactName);
	}

	private static string ReadArtifactFilePath(
		JsonObject Artifact,
		string PackageRoot,
		string ArtifactName)
	{
		if (!Artifact.TryGetStringField("file", out string RelativePath))
		{
			throw new BuildException($"AvidScript artifact '{ArtifactName}' has no file path.");
		}
		return ResolvePackagePath(PackageRoot, RelativePath, ArtifactName);
	}

	private static string ResolvePackagePath(
		string Root,
		string RelativePath,
		string Label)
	{
		if (String.IsNullOrWhiteSpace(RelativePath)
			|| Path.IsPathRooted(RelativePath)
			|| RelativePath.Split('/', '\\').Contains(".."))
		{
			throw new BuildException($"AvidScript {Label} path is invalid.");
		}
		string FullRoot = Path.GetFullPath(Root)
			.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)
			+ Path.DirectorySeparatorChar;
		string FullPath = Path.GetFullPath(Path.Combine(Root, RelativePath));
		if (!FullPath.StartsWith(FullRoot, StringComparison.OrdinalIgnoreCase)
			|| !File.Exists(FullPath))
		{
			throw new BuildException($"AvidScript {Label} file is missing or escapes its package root.");
		}
		return FullPath;
	}

	private static JsonObject ReadJson(string Path, string Label)
	{
		try
		{
			return JsonObject.Read(new FileReference(Path));
		}
		catch (Exception Error)
		{
			throw new BuildException($"AvidScript {Label} could not be parsed: {Error.Message}");
		}
	}

	private static bool IsNormalizedModuleId(string Value)
	{
		return Value.Length >= 1
			&& Value.Length <= 64
			&& Value[0] >= 'a'
			&& Value[0] <= 'z'
			&& Value.All(Character =>
				(Character >= 'a' && Character <= 'z')
				|| (Character >= '0' && Character <= '9')
				|| Character == '_'
				|| Character == '.'
				|| Character == '-');
	}

	private static bool IsLowercaseSha256(string Value)
	{
		return Value.Length == 64 && Value.All(Character =>
			(Character >= '0' && Character <= '9')
			|| (Character >= 'a' && Character <= 'f'));
	}
}
