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
		StageStartupScenarios(Target);
	}

	private void StageStartupScenarios(ReadOnlyTargetRules Target)
	{
		if (Target.ProjectFile == null)
		{
			return;
		}

		string ProjectRoot = Path.GetDirectoryName(Target.ProjectFile.FullName)!;
		string PluginRoot = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", ".."));
		foreach (string ScenarioRoot in new[]
		{
			Path.Combine(ProjectRoot, "Content", "AvidScript", "Startup"),
			Path.Combine(PluginRoot, "Content", "AvidScript", "Startup")
		})
		{
			if (!Directory.Exists(ScenarioRoot))
			{
				continue;
			}
			foreach (string ScenarioPath in Directory.GetFiles(
				ScenarioRoot,
				"*.json",
				SearchOption.AllDirectories).OrderBy(Path => Path, StringComparer.OrdinalIgnoreCase))
			{
				ExternalDependencies.Add(ScenarioPath);
				RuntimeDependencies.Add(ScenarioPath, StagedFileType.UFS);
			}
		}
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
			|| (SchemaVersion != 1 && SchemaVersion != 2)
			|| !Catalog.TryGetObjectArrayField("modules", out JsonObject[] Modules))
		{
			throw new BuildException("AvidScript module catalog schema is invalid.");
		}

		string ExpectedPlatform = Target.Platform == UnrealTargetPlatform.Win64
			? "win64"
			: Target.Platform == UnrealTargetPlatform.Android
				? "android"
				: Target.Platform == UnrealTargetPlatform.IOS
					? "ios"
					: "unsupported";
		string ExpectedArchitecture = ExpectedPlatform == "win64" ? "x86_64" : "arm64";
		string ExpectedConfiguration =
			Target.Configuration == UnrealTargetConfiguration.Shipping
				? "shipping"
				: "development";
		if (ExpectedPlatform == "unsupported")
		{
			throw new BuildException(
				$"AvidScript has no module package platform identity for {Target.Platform}.");
		}
		HashSet<string> StagedFiles = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
		{
			CatalogPath
		};
		string PreviousModuleId = String.Empty;
		foreach (JsonObject Module in Modules)
		{
			if (!Module.TryGetStringField("module_id", out string ModuleId)
				|| !IsNormalizedModuleId(ModuleId)
				|| (!String.IsNullOrEmpty(PreviousModuleId)
					&& String.CompareOrdinal(PreviousModuleId, ModuleId) >= 0))
			{
				throw new BuildException(
					"AvidScript module catalog modules are invalid, duplicated, or unsorted.");
			}
			PreviousModuleId = ModuleId;

			List<PublishedModuleVariant> Variants = ReadCatalogVariants(
				Module,
				ModuleId,
				SchemaVersion);
			PublishedModuleVariant[] Matches = Variants.Where(Variant =>
				Variant.Platform == ExpectedPlatform
				&& Variant.Architecture == ExpectedArchitecture
				&& Variant.Configuration == ExpectedConfiguration
				&& Variant.Backend == "wasmtime"
				&& Variant.Format == "wasmtime_serialized_v1").ToArray();
			if (Matches.Length > 1)
			{
				throw new BuildException(
					$"AvidScript module '{ModuleId}' has multiple variants for "
					+ $"{ExpectedPlatform}/{ExpectedArchitecture}/{ExpectedConfiguration}/wasmtime.");
			}
			if (Matches.Length == 0)
			{
				continue;
			}
			PublishedModuleVariant Selected = Matches[0];
			string PackageId = Selected.PackageId;
			string DescriptorFile = Selected.DescriptorFile;
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
				|| !Descriptor.TryGetStringField("platform", out string DescriptorPlatform)
				|| !Descriptor.TryGetStringField("configuration", out string DescriptorConfiguration)
				|| DescriptorModuleId != ModuleId
				|| DescriptorPackageId != PackageId
				|| DescriptorPlatform != Selected.Platform
				|| DescriptorConfiguration != Selected.Configuration
				|| !Descriptor.TryGetObjectField("execution", out JsonObject Execution)
				|| !Execution.TryGetStringField("backend", out string DescriptorBackend)
				|| !Execution.TryGetStringField("format", out string DescriptorFormat)
				|| DescriptorBackend != Selected.Backend
				|| DescriptorFormat != Selected.Format
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

	private static List<PublishedModuleVariant> ReadCatalogVariants(
		JsonObject Module,
		string ModuleId,
		int SchemaVersion)
	{
		if (SchemaVersion == 1)
		{
			return new List<PublishedModuleVariant>
			{
				ReadCatalogVariant(Module, ModuleId, true)
			};
		}
		if (!Module.TryGetObjectArrayField("variants", out JsonObject[] VariantObjects)
			|| VariantObjects.Length == 0)
		{
			throw new BuildException(
				$"AvidScript module '{ModuleId}' has no catalog variants.");
		}

		List<PublishedModuleVariant> Variants = new List<PublishedModuleVariant>();
		string PreviousKey = String.Empty;
		foreach (JsonObject VariantObject in VariantObjects)
		{
			PublishedModuleVariant Variant = ReadCatalogVariant(
				VariantObject,
				ModuleId,
				false);
			string Key = Variant.IdentityKey;
			if (!String.IsNullOrEmpty(PreviousKey)
				&& String.CompareOrdinal(PreviousKey, Key) >= 0)
			{
				throw new BuildException(
					$"AvidScript module '{ModuleId}' variants are duplicated or unsorted.");
			}
			PreviousKey = Key;
			Variants.Add(Variant);
		}
		return Variants;
	}

	private static PublishedModuleVariant ReadCatalogVariant(
		JsonObject Variant,
		string ModuleId,
		bool Legacy)
	{
		string Architecture = Legacy ? "x86_64" : ReadRequiredString(Variant, "architecture");
		string Backend = Legacy ? "wasmtime" : ReadRequiredString(Variant, "backend");
		string Format = Legacy ? "wasmtime_serialized_v1" : ReadRequiredString(Variant, "format");
		string Platform = ReadRequiredString(Variant, "platform");
		string Configuration = ReadRequiredString(Variant, "configuration");
		string PackageId = ReadRequiredString(Variant, "package_id");
		string DescriptorFile = ReadRequiredString(Variant, "descriptor_file");
		string DescriptorSha256 = ReadRequiredString(Variant, "descriptor_sha256");
		bool PlatformIdentityValid =
			(Platform == "win64" && Architecture == "x86_64")
			|| (Platform == "android" && Architecture == "arm64")
			|| (Platform == "ios" && Architecture == "arm64");
		bool BackendFormatValid =
			(Backend == "wasmtime" && Format == "wasmtime_serialized_v1")
			|| (Backend == "wamr" && Format == "wamr_aot_v1");
		if (!PlatformIdentityValid
			|| (Configuration != "development" && Configuration != "shipping")
			|| !BackendFormatValid
			|| !IsLowercaseSha256(PackageId)
			|| !IsLowercaseSha256(DescriptorSha256)
			|| DescriptorFile != $"{ModuleId}/{PackageId}/package.json")
		{
			throw new BuildException(
				$"AvidScript module '{ModuleId}' contains an invalid catalog variant.");
		}
		return new PublishedModuleVariant(
			Platform,
			Architecture,
			Configuration,
			Backend,
			Format,
			PackageId,
			DescriptorFile);
	}

	private static string ReadRequiredString(JsonObject Object, string Field)
	{
		if (!Object.TryGetStringField(Field, out string Value)
			|| String.IsNullOrWhiteSpace(Value))
		{
			throw new BuildException($"AvidScript catalog field '{Field}' is invalid.");
		}
		return Value;
	}

	private sealed class PublishedModuleVariant
	{
		public PublishedModuleVariant(
			string Platform,
			string Architecture,
			string Configuration,
			string Backend,
			string Format,
			string PackageId,
			string DescriptorFile)
		{
			this.Platform = Platform;
			this.Architecture = Architecture;
			this.Configuration = Configuration;
			this.Backend = Backend;
			this.Format = Format;
			this.PackageId = PackageId;
			this.DescriptorFile = DescriptorFile;
		}

		public string Platform { get; }
		public string Architecture { get; }
		public string Configuration { get; }
		public string Backend { get; }
		public string Format { get; }
		public string PackageId { get; }
		public string DescriptorFile { get; }
		public string IdentityKey => String.Join(
			"\n",
			Platform,
			Architecture,
			Configuration,
			Backend,
			Format);
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
