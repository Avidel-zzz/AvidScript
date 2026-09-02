using System.IO;
using System.Security.Cryptography;
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
