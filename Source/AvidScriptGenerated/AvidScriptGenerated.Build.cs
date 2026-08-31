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

		StageGeneratedTypeCookPackage();
	}

	private void StageGeneratedTypeCookPackage()
	{
		string PluginRoot = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", ".."));
		string GeneratedRoot = Path.Combine(PluginRoot, "Content", "AvidScriptGenerated");
		string CurrentDescriptor = Path.Combine(GeneratedRoot, "current.json");
		if (!File.Exists(CurrentDescriptor))
		{
			return;
		}

		string PackageId;
		try
		{
			JsonObject Document = JsonObject.Read(new FileReference(CurrentDescriptor));
			if (!Document.TryGetIntegerField("schema_version", out int SchemaVersion)
				|| SchemaVersion != 1
				|| !Document.TryGetStringField("package_id", out string PackageIdValue))
			{
				throw new BuildException("AvidScript generated Cook pointer schema is invalid.");
			}
			PackageId = PackageIdValue;
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

		if (!IsLowercaseSha256(PackageId))
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
		RuntimeDependencies.Add(CurrentDescriptor, StagedFileType.NonUFS);
		for (int Index = 0; Index < BundleFiles.Length; ++Index)
		{
			ExternalDependencies.Add(BundleFiles[Index]);
			RuntimeDependencies.Add(BundleFiles[Index], StagedFileType.NonUFS);
		}
	}

	private static bool IsLowercaseSha256(string Value)
	{
		return Value.Length == 64 && Value.All(Character =>
			(Character >= '0' && Character <= '9')
			|| (Character >= 'a' && Character <= 'f'));
	}
}
