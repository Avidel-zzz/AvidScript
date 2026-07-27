using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using UnrealBuildTool;

public class WAMR : ModuleRules
{
	public WAMR(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.External;

		string UpstreamDir = Path.Combine(ModuleDirectory, "upstream");
		string IncludeDir = Path.Combine(UpstreamDir, "core", "iwasm", "include");
		string LibraryPath = FindLibraryPath(Target);

		bool bHasHeaders = Directory.Exists(IncludeDir);
		bool bHasLibrary = !string.IsNullOrEmpty(LibraryPath) && File.Exists(LibraryPath);
		bool bEnableWamr = bHasHeaders && bHasLibrary;

		PublicDefinitions.Add(bEnableWamr ? "AVIDSCRIPT_WITH_WAMR=1" : "AVIDSCRIPT_WITH_WAMR=0");
		PublicDefinitions.Add(
			bEnableWamr
				? "AVIDSCRIPT_WAMR_INTERPRETER_CONFIG=\"interp=1,fast_interp=1,aot=0,jit=0,fast_jit=0,simd=1,simde=1\""
				: "AVIDSCRIPT_WAMR_INTERPRETER_CONFIG=\"unavailable\"");
		PublicDefinitions.Add(
			bEnableWamr
				? $"AVIDSCRIPT_WAMR_STATIC_LIB_SHA256=\"{ComputeFileSha256(LibraryPath)}\""
				: "AVIDSCRIPT_WAMR_STATIC_LIB_SHA256=\"unavailable\"");

		if (bEnableWamr)
		{
			PublicIncludePaths.Add(IncludeDir);
			PublicAdditionalLibraries.Add(LibraryPath);
			PublicDefinitions.Add("WASM_RUNTIME_API_EXTERN=");

			if (Target.Platform == UnrealTargetPlatform.Win64)
			{
				PublicSystemLibraries.Add("ntdll.lib");
			}
		}
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

	private string FindLibraryPath(ReadOnlyTargetRules Target)
	{
		foreach (string Candidate in GetLibraryCandidates(Target))
		{
			if (File.Exists(Candidate))
			{
				return Candidate;
			}
		}

		return string.Empty;
	}

	private IEnumerable<string> GetLibraryCandidates(ReadOnlyTargetRules Target)
	{
		string PlatformName = Target.Platform == UnrealTargetPlatform.Win64 ? "Win64" : Target.Platform.ToString();
		string LibraryDir = Path.Combine(ModuleDirectory, "lib", PlatformName, "Release");

		yield return Path.Combine(LibraryDir, "iwasm.lib");
		yield return Path.Combine(LibraryDir, "libiwasm.lib");
		yield return Path.Combine(LibraryDir, "vmlib.lib");
	}
}
