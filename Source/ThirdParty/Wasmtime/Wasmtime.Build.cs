using System.IO;
using System.Security.Cryptography;
using UnrealBuildTool;

public class Wasmtime : ModuleRules
{
	public Wasmtime(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.External;

		string PerformanceInstallRoot = Path.Combine(
			ModuleDirectory,
			"installed",
			"Win64",
			"v45.0.0-avidscript.1");
		string OfficialInstallRoot = Path.Combine(
			ModuleDirectory,
			"installed",
			"Win64",
			"v45.0.0");
		bool bHasPerformanceLayout =
			Target.Platform == UnrealTargetPlatform.Win64 &&
			HasManagedLayout(
				PerformanceInstallRoot,
				".avidscript-wasmtime-performance-managed.json");
		string InstallRoot = bHasPerformanceLayout
			? PerformanceInstallRoot
			: OfficialInstallRoot;
		string IncludePath = Path.Combine(InstallRoot, "include");
		string HeaderPath = Path.Combine(IncludePath, "wasmtime.h");
		string DllPath = Path.Combine(InstallRoot, "lib", "wasmtime.dll");
		string ImportLibraryPath = Path.Combine(InstallRoot, "lib", "wasmtime.dll.lib");
		string LicensePath = Path.Combine(InstallRoot, "LICENSE");
		string MarkerPath = Path.Combine(
			InstallRoot,
			bHasPerformanceLayout
				? ".avidscript-wasmtime-performance-managed.json"
				: ".avidscript-wasmtime-managed.json");

		bool bHasManagedLayout =
			Target.Platform == UnrealTargetPlatform.Win64 &&
			Directory.Exists(IncludePath) &&
			File.Exists(HeaderPath) &&
			File.Exists(DllPath) &&
			File.Exists(ImportLibraryPath) &&
			File.Exists(LicensePath) &&
			File.Exists(MarkerPath);
		bool bPackagedRuntimeTarget =
			Target.Type == TargetType.Game
			|| Target.Type == TargetType.Client
			|| Target.Type == TargetType.Server;
		if (Target.Platform == UnrealTargetPlatform.Win64
			&& bPackagedRuntimeTarget
			&& !bHasPerformanceLayout)
		{
			throw new BuildException(
				"AvidScript packaged Win64 targets require the managed Wasmtime performance toolchain.");
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
			bHasManagedLayout
				? $"AVIDSCRIPT_WASMTIME_DLL_SHA256=\"{ComputeFileSha256(DllPath)}\""
				: "AVIDSCRIPT_WASMTIME_DLL_SHA256=\"unavailable\"");

		if (bHasManagedLayout)
		{
			ExternalDependencies.Add(MarkerPath);
			ExternalDependencies.Add(DllPath);
			ExternalDependencies.Add(ImportLibraryPath);
			PublicIncludePaths.Add(IncludePath);
			PublicAdditionalLibraries.Add(ImportLibraryPath);
			PublicDelayLoadDLLs.Add("wasmtime.dll");
			RuntimeDependencies.Add("$(PluginDir)/Binaries/Win64/wasmtime.dll", DllPath, StagedFileType.NonUFS);
			RuntimeDependencies.Add("$(PluginDir)/Binaries/Win64/wasmtime.LICENSE.txt", LicensePath, StagedFileType.NonUFS);
		}
	}

	private static bool HasManagedLayout(string InstallRoot, string MarkerName)
	{
		return
			Directory.Exists(Path.Combine(InstallRoot, "include")) &&
			File.Exists(Path.Combine(InstallRoot, "include", "wasmtime.h")) &&
			File.Exists(Path.Combine(InstallRoot, "lib", "wasmtime.dll")) &&
			File.Exists(Path.Combine(InstallRoot, "lib", "wasmtime.dll.lib")) &&
			File.Exists(Path.Combine(InstallRoot, "LICENSE")) &&
			File.Exists(Path.Combine(InstallRoot, MarkerName));
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
