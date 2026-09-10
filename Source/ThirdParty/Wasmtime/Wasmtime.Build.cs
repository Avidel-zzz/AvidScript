using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.RegularExpressions;
using UnrealBuildTool;

public class Wasmtime : ModuleRules
{
	public Wasmtime(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.External;

		bool bWin64Target = Target.Platform == UnrealTargetPlatform.Win64;
		bool bAndroidTarget = Target.Platform == UnrealTargetPlatform.Android;
		string PerformanceInstallRoot = Path.Combine(
			ModuleDirectory,
			"installed",
			"Win64",
			"v45.0.0-avidscript.2");
		string OfficialInstallRoot = Path.Combine(
			ModuleDirectory,
			"installed",
			"Win64",
			"v45.0.0");
		bool bHasPerformanceLayout = bWin64Target &&
			HasWin64ManagedLayout(
				PerformanceInstallRoot,
				".avidscript-wasmtime-performance-managed.json");
		string Win64InstallRoot = bHasPerformanceLayout
			? PerformanceInstallRoot
			: OfficialInstallRoot;
		string Win64DllPath = Path.Combine(
			Win64InstallRoot,
			"lib",
			"wasmtime.dll");
		string Win64MarkerName =
			bHasPerformanceLayout
				? ".avidscript-wasmtime-performance-managed.json"
				: ".avidscript-wasmtime-managed.json";
		bool bHasWin64Layout = bWin64Target
			&& HasWin64ManagedLayout(Win64InstallRoot, Win64MarkerName);

		string AndroidInstallRoot = Path.Combine(
			ModuleDirectory,
			"installed",
			"Android",
			"arm64",
			"v45.0.0");
		bool bHasAndroidCrossTargetLayout =
			HasAndroidManagedLayout(AndroidInstallRoot);
		bool bHasAndroidLayout = bAndroidTarget
			&& bHasAndroidCrossTargetLayout;
		bool bHasManagedLayout = bHasWin64Layout || bHasAndroidLayout;
		bool bPackagedRuntimeTarget =
			Target.Type == TargetType.Game
			|| Target.Type == TargetType.Client
			|| Target.Type == TargetType.Server;
		if (bWin64Target
			&& bPackagedRuntimeTarget
			&& !bHasPerformanceLayout)
		{
			throw new BuildException(
				"AvidScript packaged Win64 targets require the managed Wasmtime performance toolchain.");
		}
		if (bAndroidTarget && Target.Architecture != UnrealArch.Arm64)
		{
			throw new BuildException(
				$"AvidScript Wasmtime supports Android arm64 only, got {Target.Architecture}.");
		}
		if (bAndroidTarget && !bHasAndroidLayout)
		{
			throw new BuildException(
				"AvidScript Android arm64 targets require the managed Wasmtime v45 Android dependency. "
				+ "Run Build/InstallWasmtimeDependency.ps1 -Mode Install -Platform AndroidArm64.");
		}

		PublicDefinitions.Add(
			bHasManagedLayout
				? "AVIDSCRIPT_WITH_WASMTIME=1"
				: "AVIDSCRIPT_WITH_WASMTIME=0");
		PublicDefinitions.Add(
			bHasPerformanceLayout
				? "AVIDSCRIPT_WITH_WASMTIME_PERFORMANCE_TOOLCHAIN=1"
				: "AVIDSCRIPT_WITH_WASMTIME_PERFORMANCE_TOOLCHAIN=0");
		PublicDefinitions.Add(
			bHasWin64Layout
				? $"AVIDSCRIPT_WASMTIME_DLL_SHA256=\"{ComputeFileSha256(Win64DllPath)}\""
				: "AVIDSCRIPT_WASMTIME_DLL_SHA256=\"unavailable\"");
		PublicDefinitions.Add(
			bHasAndroidCrossTargetLayout
				? $"AVIDSCRIPT_WASMTIME_ANDROID_STATIC_SHA256=\"{ComputeFileSha256(Path.Combine(AndroidInstallRoot, "lib", "libwasmtime.a"))}\""
				: "AVIDSCRIPT_WASMTIME_ANDROID_STATIC_SHA256=\"unavailable\"");

		if (bHasWin64Layout)
		{
			string IncludePath = Path.Combine(Win64InstallRoot, "include");
			string ImportLibraryPath = Path.Combine(
				Win64InstallRoot,
				"lib",
				"wasmtime.dll.lib");
			string LicensePath = Path.Combine(Win64InstallRoot, "LICENSE");
			string MarkerPath = Path.Combine(Win64InstallRoot, Win64MarkerName);
			ExternalDependencies.Add(MarkerPath);
			ExternalDependencies.Add(Win64DllPath);
			ExternalDependencies.Add(ImportLibraryPath);
			PublicIncludePaths.Add(IncludePath);
			PublicAdditionalLibraries.Add(ImportLibraryPath);
			PublicDelayLoadDLLs.Add("wasmtime.dll");
			RuntimeDependencies.Add("$(PluginDir)/Binaries/Win64/wasmtime.dll", Win64DllPath, StagedFileType.NonUFS);
			RuntimeDependencies.Add("$(PluginDir)/Binaries/Win64/wasmtime.LICENSE.txt", LicensePath, StagedFileType.NonUFS);
			StageWin64Notices(Win64InstallRoot, bPackagedRuntimeTarget);
		}
		else if (bHasAndroidLayout)
		{
			string IncludePath = Path.Combine(AndroidInstallRoot, "include");
			string StaticLibraryPath = Path.Combine(
				AndroidInstallRoot,
				"lib",
				"libwasmtime.a");
			string LicensePath = Path.Combine(AndroidInstallRoot, "LICENSE");
			string MarkerPath = Path.Combine(
				AndroidInstallRoot,
				".avidscript-wasmtime-managed.json");
			ExternalDependencies.Add(MarkerPath);
			ExternalDependencies.Add(StaticLibraryPath);
			PublicIncludePaths.Add(IncludePath);
			PublicAdditionalLibraries.Add(StaticLibraryPath);
			RuntimeDependencies.Add(
				"$(PluginDir)/Binaries/Android/wasmtime.LICENSE.txt",
				LicensePath,
				StagedFileType.NonUFS);
		}
	}

	private void StageWin64Notices(string InstallRoot, bool Required)
	{
		string Root = Path.Combine(InstallRoot, "notices");
		string ManifestPath = Path.Combine(Root, "manifest.json");
		ExternalDependencies.Add(ManifestPath);
		if (!Directory.Exists(Root) && !File.Exists(Root) && !Required)
		{
			return;
		}
		try
		{
			// Validate the entire ordinary tree before registering any notice for staging.
			SortedSet<string> ActualFiles = new SortedSet<string>(StringComparer.Ordinal);
			Stack<string> Pending = new Stack<string>();
			Pending.Push(Root);
			int EntryCount = 0;
			long TotalBytes = 0;
			while (Pending.Count > 0)
			{
				string Entry = Pending.Pop();
				FileAttributes Attributes = File.GetAttributes(Entry);
				if (++EntryCount > 65536 || (Attributes & FileAttributes.ReparsePoint) != 0)
				{
					throw new InvalidDataException("notice tree contains a link or exceeds its entry limit");
				}
				if ((Attributes & FileAttributes.Directory) != 0)
				{
					foreach (string Child in Directory.EnumerateFileSystemEntries(Entry))
					{
						if (Pending.Count + EntryCount >= 65536)
						{
							throw new InvalidDataException("notice tree exceeds its entry limit");
						}
						Pending.Push(Child);
					}
				}
				else
				{
					long Length = new FileInfo(Entry).Length;
					TotalBytes += Length;
					if (Length < 1 || Length > 16 * 1024 * 1024 || TotalBytes > 256 * 1024 * 1024)
					{
						throw new InvalidDataException("notice tree exceeds its byte limits or contains an empty file");
					}
					ActualFiles.Add(Path.GetRelativePath(Root, Entry).Replace('\\', '/'));
				}
			}
			using (FileStream ManifestStream = File.OpenRead(ManifestPath))
			{
				// Avoid JsonDocument.Parse overloads requiring System.Memory in UBT RulesAssembly.
				// Keep JsonElement so duplicate JSON properties are rejected before field lookup.
				JsonElement Manifest = JsonSerializer.Deserialize<JsonElement>(ManifestStream,
					new JsonSerializerOptions { MaxDepth = 32 });
				AssertUniqueNoticeJsonProperties(Manifest);
				JsonElement Packages = Manifest.GetProperty("packages");
				int PackageCount = Manifest.GetProperty("package_count").GetInt32();
				if (Manifest.GetProperty("schema_version").GetInt32() != 1
					|| Manifest.GetProperty("scope").GetString() != "win64-wasmtime-build-notices"
					|| PackageCount < 1 || PackageCount > 512 || Packages.GetArrayLength() != PackageCount)
				{
					throw new InvalidDataException("notice manifest version or package inventory is invalid");
				}
				foreach (string Library in new[] { "wasmtime.dll", "wasmtime.dll.lib", "wasmtime.lib" })
				{
					if (Manifest.GetProperty("runtime").GetProperty(Library).GetString()
						!= ComputeFileSha256(Path.Combine(InstallRoot, "lib", Library)))
					{
						throw new InvalidDataException("notice manifest belongs to a different runtime");
					}
				}
				SortedSet<string> ExpectedFiles = new SortedSet<string>(StringComparer.Ordinal) { "manifest.json" };
				HashSet<string> PackageIds = new HashSet<string>(StringComparer.Ordinal);
				foreach (JsonElement Package in Packages.EnumerateArray())
				{
					string Id = Package.GetProperty("id").GetString() ?? "";
					if (Id.Length == 0 || !PackageIds.Add(Id))
					{
						throw new InvalidDataException("notice package identity is empty or duplicated");
					}
					ValidateNoticeReferences(Root, Package.GetProperty("notices"), ExpectedFiles);
				}
				ValidateNoticeReferences(Root, Manifest.GetProperty("rust").GetProperty("notices"), ExpectedFiles);
				if (!ExpectedFiles.SetEquals(ActualFiles))
				{
					throw new InvalidDataException("notice bundle contains missing or unreferenced files");
				}
				foreach (string Relative in ExpectedFiles)
				{
					string Source = Path.Combine(Root, Relative);
					if (Relative != "manifest.json")
					{
						ExternalDependencies.Add(Source);
					}
					RuntimeDependencies.Add("$(PluginDir)/Binaries/Win64/wasmtime.notices/" + Relative,
						Source, StagedFileType.NonUFS);
				}
			}
		}
		catch (Exception Error)
		{
			throw new BuildException("AvidScript Win64 Wasmtime notices are missing or invalid: "
				+ Error.Message + ". Build the managed toolchain with -NeutralBuild before packaging.");
		}
	}

	private static void AssertUniqueNoticeJsonProperties(JsonElement Value)
	{
		if (Value.ValueKind == JsonValueKind.Object)
		{
			HashSet<string> Names = new HashSet<string>(StringComparer.Ordinal);
			foreach (JsonProperty Property in Value.EnumerateObject())
			{
				if (!Names.Add(Property.Name))
				{
					throw new InvalidDataException("duplicate notice JSON property");
				}
				AssertUniqueNoticeJsonProperties(Property.Value);
			}
		}
		else if (Value.ValueKind == JsonValueKind.Array)
		{
			foreach (JsonElement Item in Value.EnumerateArray())
			{
				AssertUniqueNoticeJsonProperties(Item);
			}
		}
	}

	private static void ValidateNoticeReferences(string Root, JsonElement Notices, SortedSet<string> ExpectedFiles)
	{
		if (Notices.GetArrayLength() < 1 || Notices.GetArrayLength() > 128)
		{
			throw new InvalidDataException("notice references are empty or excessive");
		}
		foreach (JsonElement Notice in Notices.EnumerateArray())
		{
			string Relative = Notice.GetProperty("path").GetString() ?? "";
			string Hash = Notice.GetProperty("sha256").GetString() ?? "";
			long Length = Notice.GetProperty("length").GetInt64();
			if (!Regex.IsMatch(Relative, @"\Atexts/[0-9a-f]{64}\.(txt|html)\z")
				|| Path.GetFileNameWithoutExtension(Relative) != Hash || Length < 1 || Length > 16 * 1024 * 1024)
			{
				throw new InvalidDataException("notice reference path or identity is invalid");
			}
			string FilePath = Path.Combine(Root, Relative);
			if (new FileInfo(FilePath).Length != Length || ComputeFileSha256(FilePath) != Hash)
			{
				throw new InvalidDataException("notice content identity differs");
			}
			ExpectedFiles.Add(Relative);
		}
	}

	private static bool HasWin64ManagedLayout(string InstallRoot, string MarkerName)
	{
		return
			Directory.Exists(Path.Combine(InstallRoot, "include")) &&
			File.Exists(Path.Combine(InstallRoot, "include", "wasmtime.h")) &&
			File.Exists(Path.Combine(InstallRoot, "lib", "wasmtime.dll")) &&
			File.Exists(Path.Combine(InstallRoot, "lib", "wasmtime.dll.lib")) &&
			File.Exists(Path.Combine(InstallRoot, "LICENSE")) &&
			File.Exists(Path.Combine(InstallRoot, MarkerName));
	}

	private static bool HasAndroidManagedLayout(string InstallRoot)
	{
		return
			Directory.Exists(Path.Combine(InstallRoot, "include")) &&
			File.Exists(Path.Combine(InstallRoot, "include", "wasmtime.h")) &&
			File.Exists(Path.Combine(InstallRoot, "lib", "libwasmtime.a")) &&
			File.Exists(Path.Combine(InstallRoot, "LICENSE")) &&
			File.Exists(Path.Combine(
				InstallRoot,
				".avidscript-wasmtime-managed.json"));
	}

	private static string ComputeFileSha256(string Path)
	{
		using (SHA256 Hasher = SHA256.Create())
		using (FileStream Stream = File.OpenRead(Path))
		{
			return System.BitConverter.ToString(Hasher.ComputeHash(Stream))
				.Replace("-", "")
				.ToLowerInvariant();
		}
	}
}
