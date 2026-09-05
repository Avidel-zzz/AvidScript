using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using EpicGames.Core;
using UnrealBuildTool;

public class AvidScriptGenerated : ModuleRules
{
	public AvidScriptGenerated(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new[]
			{
				"Core",
				"CoreUObject",
				"Engine"
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new[]
			{
				"AvidScriptRuntime",
				"Json",
				"Projects"
			}
		);

		ConfigureGeneratedTypeCompilation();
		StageGeneratedTypeCookPackage(Target);
	}

	private void ConfigureGeneratedTypeCompilation()
	{
		string GeneratedHeader = Path.Combine(
			ModuleDirectory,
			"Public",
			"AvidScriptGeneratedTypes.h");
		string GeneratedSource = Path.Combine(
			ModuleDirectory,
			"Private",
			"AvidScriptGeneratedTypes.cpp");
		bool bHasGeneratedHeader = File.Exists(GeneratedHeader);
		bool bHasGeneratedSource = File.Exists(GeneratedSource);
		if (bHasGeneratedHeader != bHasGeneratedSource)
		{
			throw new BuildException(
				"AvidScript generated type header and source must be generated together.");
		}

		bool bHasGeneratedTypes = bHasGeneratedHeader && bHasGeneratedSource;
		PrivateDefinitions.Add(
			$"AVIDSCRIPT_WITH_GENERATED_TYPES={(bHasGeneratedTypes ? 1 : 0)}");
		if (bHasGeneratedTypes)
		{
			ExternalDependencies.Add(GeneratedHeader);
			ExternalDependencies.Add(GeneratedSource);
		}
	}

	private void StageGeneratedTypeCookPackage(ReadOnlyTargetRules Target)
	{
		string PluginRoot = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", ".."));
		string GeneratedRoot = Path.Combine(PluginRoot, "Content", "AvidScriptGenerated");
		string CurrentDescriptor = Path.Combine(GeneratedRoot, "current.json");
		if (!File.Exists(CurrentDescriptor))
		{
			if (Target.Type != TargetType.Editor)
			{
				throw new BuildException(
					"AvidScript packaged targets require Content/AvidScriptGenerated/current.json.");
			}
			return;
		}

		int SchemaVersion;
		JsonObject Document;
		try
		{
			Document = JsonObject.Read(new FileReference(CurrentDescriptor));
			if (!Document.TryGetIntegerField("schema_version", out SchemaVersion)
				|| (SchemaVersion != 1 && SchemaVersion != 2))
			{
				throw new BuildException("AvidScript generated Cook pointer schema is invalid.");
			}
		}
		catch (BuildException)
		{
			throw;
		}
		catch (Exception Error)
		{
			throw new BuildException(
				$"AvidScript generated Cook pointer could not be parsed: {Error.Message}");
		}

		if (SchemaVersion == 2)
		{
			StageGeneratedTypeV2(Document, GeneratedRoot, CurrentDescriptor);
			return;
		}
		if (Target.Type != TargetType.Editor)
		{
			throw new BuildException(
				"AvidScript packaged targets require Generated Type Cook pointer schema v2.");
		}
		StageGeneratedTypeV1(Document, GeneratedRoot, CurrentDescriptor);
	}

	private void StageGeneratedTypeV2(
		JsonObject Document,
		string GeneratedRoot,
		string CurrentDescriptor)
	{
		if (!Document.TryGetStringField("module_id", out string ModuleId)
			|| !Document.TryGetStringField("package_id", out string PackageId)
			|| !IsNormalizedModuleId(ModuleId)
			|| !IsLowercaseSha256(PackageId)
			|| !Document.TryGetObjectField("type_manifest", out JsonObject TypeManifest)
			|| !TypeManifest.TryGetStringField("file", out string TypeManifestFile)
			|| !TypeManifest.TryGetStringField("sha256", out string TypeManifestSha256)
			|| !IsLowercaseSha256(TypeManifestSha256))
		{
			throw new BuildException("AvidScript generated Cook pointer v2 is invalid.");
		}

		string TypeManifestPath = ResolveGeneratedPath(
			GeneratedRoot,
			TypeManifestFile,
			"type manifest");
		string BundleRoot = Path.GetDirectoryName(TypeManifestPath)!;
		string[] BundleFiles = Directory.EnumerateFiles(
			BundleRoot,
			"*",
			SearchOption.AllDirectories).ToArray();
		if (BundleFiles.Length != 1
			|| !String.Equals(
				Path.GetFileName(BundleFiles[0]),
				"type-manifest.json",
				StringComparison.Ordinal))
		{
			throw new BuildException(
				"AvidScript Generated Type v2 bundle must contain only type-manifest.json.");
		}

		ExternalDependencies.Add(CurrentDescriptor);
		ExternalDependencies.Add(TypeManifestPath);
		RuntimeDependencies.Add(CurrentDescriptor, StagedFileType.UFS);
		RuntimeDependencies.Add(TypeManifestPath, StagedFileType.UFS);
	}

	private void StageGeneratedTypeV1(
		JsonObject Document,
		string GeneratedRoot,
		string CurrentDescriptor)
	{
		if (!Document.TryGetStringField("package_id", out string PackageId)
			|| !IsLowercaseSha256(PackageId))
		{
			throw new BuildException("AvidScript generated Cook package id is invalid.");
		}

		string BundleRoot = Path.GetFullPath(Path.Combine(GeneratedRoot, PackageId));
		string GeneratedRootBoundary = Path.GetFullPath(GeneratedRoot)
			.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)
			+ Path.DirectorySeparatorChar;
		if (!BundleRoot.StartsWith(GeneratedRootBoundary, StringComparison.OrdinalIgnoreCase)
			|| !Directory.Exists(BundleRoot))
		{
			throw new BuildException("AvidScript generated Cook package directory is missing or invalid.");
		}

		string[] RequiredFiles =
		{
			"bindings/bindings.json",
			"bindings/package.json",
			"generated_types.debug.json",
			"generated_types.wasm",
			"runtime-manifest.json",
			"type-manifest.json"
		};
		HashSet<string> RequiredSet = new HashSet<string>(RequiredFiles, StringComparer.Ordinal);
		string[] BundleFiles = Directory.EnumerateFiles(
			BundleRoot,
			"*",
			SearchOption.AllDirectories)
			.OrderBy(Path => Path, StringComparer.Ordinal)
			.ToArray();
		string[] RelativeBundleFiles = BundleFiles
			.Select(Path => System.IO.Path.GetRelativePath(BundleRoot, Path).Replace('\\', '/'))
			.ToArray();
		if (RelativeBundleFiles.Length != RequiredFiles.Length
			|| RelativeBundleFiles.Any(Path => !RequiredSet.Contains(Path)))
		{
			throw new BuildException(
				"AvidScript generated Cook package must contain exactly the six published artifacts.");
		}

		ExternalDependencies.Add(CurrentDescriptor);
		RuntimeDependencies.Add(CurrentDescriptor, StagedFileType.UFS);
		for (int Index = 0; Index < BundleFiles.Length; ++Index)
		{
			ExternalDependencies.Add(BundleFiles[Index]);
			RuntimeDependencies.Add(BundleFiles[Index], StagedFileType.UFS);
		}
	}

	private static string ResolveGeneratedPath(
		string GeneratedRoot,
		string RelativePath,
		string Label)
	{
		if (String.IsNullOrWhiteSpace(RelativePath)
			|| Path.IsPathRooted(RelativePath)
			|| RelativePath.Split('/', '\\').Contains(".."))
		{
			throw new BuildException($"AvidScript generated {Label} path is invalid.");
		}
		string RootBoundary = Path.GetFullPath(GeneratedRoot)
			.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)
			+ Path.DirectorySeparatorChar;
		string FullPath = Path.GetFullPath(Path.Combine(GeneratedRoot, RelativePath));
		if (!FullPath.StartsWith(RootBoundary, StringComparison.OrdinalIgnoreCase)
			|| !File.Exists(FullPath))
		{
			throw new BuildException(
				$"AvidScript generated {Label} is missing or escapes its root.");
		}
		return FullPath;
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
