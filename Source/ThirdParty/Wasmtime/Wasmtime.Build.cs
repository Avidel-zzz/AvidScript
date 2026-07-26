using System.IO;
using UnrealBuildTool;

public class Wasmtime : ModuleRules
{
	public Wasmtime(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.External;

		string InstallRoot = Path.Combine(ModuleDirectory, "installed", "Win64", "v45.0.0");
		string IncludePath = Path.Combine(InstallRoot, "include");
		string HeaderPath = Path.Combine(IncludePath, "wasmtime.h");
		string DllPath = Path.Combine(InstallRoot, "lib", "wasmtime.dll");
		string ImportLibraryPath = Path.Combine(InstallRoot, "lib", "wasmtime.dll.lib");
		string LicensePath = Path.Combine(InstallRoot, "LICENSE");
		string MarkerPath = Path.Combine(InstallRoot, ".avidscript-wasmtime-managed.json");

		bool bHasManagedLayout =
			Target.Platform == UnrealTargetPlatform.Win64 &&
			Directory.Exists(IncludePath) &&
			File.Exists(HeaderPath) &&
			File.Exists(DllPath) &&
			File.Exists(ImportLibraryPath) &&
			File.Exists(LicensePath) &&
			File.Exists(MarkerPath);

		PublicDefinitions.Add(
			bHasManagedLayout
				? "AVIDSCRIPT_WITH_WASMTIME=1"
				: "AVIDSCRIPT_WITH_WASMTIME=0");

		if (bHasManagedLayout)
		{
			PublicIncludePaths.Add(IncludePath);
			PublicAdditionalLibraries.Add(ImportLibraryPath);
			PublicDelayLoadDLLs.Add("wasmtime.dll");
			RuntimeDependencies.Add("$(PluginDir)/Binaries/Win64/wasmtime.dll", DllPath, StagedFileType.NonUFS);
		}
	}
}
