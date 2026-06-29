using System.IO;
using UnrealBuildTool;

public class WAMR : ModuleRules
{
	public WAMR(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.External;

		string UpstreamDir = Path.Combine(ModuleDirectory, "upstream");
		string IncludeDir = Path.Combine(UpstreamDir, "core", "iwasm", "include");
		string LibraryPath = GetLibraryPath(Target);

		bool bHasHeaders = Directory.Exists(IncludeDir);
		bool bHasLibrary = File.Exists(LibraryPath);
		bool bEnableWamr = bHasHeaders && bHasLibrary;

		PublicDefinitions.Add(bEnableWamr ? "AVIDSCRIPT_WITH_WAMR=1" : "AVIDSCRIPT_WITH_WAMR=0");

		if (bEnableWamr)
		{
			PublicIncludePaths.Add(IncludeDir);
			PublicAdditionalLibraries.Add(LibraryPath);
		}
	}

	private string GetLibraryPath(ReadOnlyTargetRules Target)
	{
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			return Path.Combine(ModuleDirectory, "lib", "Win64", "Release", "vmlib.lib");
		}

		return Path.Combine(ModuleDirectory, "lib", Target.Platform.ToString(), "Release", "vmlib.lib");
	}
}

