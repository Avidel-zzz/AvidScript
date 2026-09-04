#include "AvidScriptValidationPaths.h"

#include "AvidScriptHash.h"
#include "CoreGlobals.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProperties.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/Archive.h"

namespace AvidScript::Validation
{
namespace PathPrivate
{
FString FullPath(const FString& Path)
{
	FString Result = FPaths::ConvertRelativePathToFull(Path);
	FPaths::NormalizeFilename(Result);
	FPaths::CollapseRelativeDirectories(Result);
	FPaths::RemoveDuplicateSlashes(Result);
	FPaths::NormalizeDirectoryName(Result);
	return Result;
}

bool IsAbsoluteLocalPath(const FString& Path)
{
	if (Path.Len() < 3 || !FChar::IsAlpha(Path[0]) || Path[1] != TEXT(':')
		|| (Path[2] != TEXT('/') && Path[2] != TEXT('\\')))
	{
		return false;
	}
	for (int32 Index = 2; Index < Path.Len(); ++Index)
	{
		if (Path[Index] < TEXT(' ') || FString(TEXT(":*?\"<>|")).Contains(FString::Chr(Path[Index]))) { return false; }
	}
	TArray<FString> Parts;
	FString Normalized = Path.Replace(TEXT("\\"), TEXT("/"));
	Normalized.ParseIntoArray(Parts, TEXT("/"));
	for (const FString& Part : Parts)
	{
		if (Part.EndsWith(TEXT(".")) || Part.EndsWith(TEXT(" "))) { return false; }
	}
	return true;
}

bool HasSafeParents(const FString& Path)
{
	IPlatformFile& Files = IPlatformFile::GetPlatformPhysical();
	FString Current = Path;
	while (!Current.IsEmpty())
	{
		// Windows IsSymlink rejects every FILE_ATTRIBUTE_REPARSE_POINT, including junctions.
		if (Files.IsSymlink(*Current) != ESymlinkResult::NonSymlink) { return false; }
		if (Files.FileExists(*Current) && !Current.Equals(Path, ESearchCase::IgnoreCase)) { return false; }
		const FString Parent = FPaths::GetPath(Current);
		if (Parent == Current) { break; }
		Current = Parent;
	}
	return true;
}

bool Overlaps(const FString& Left, const FString& Right)
{
	return Left.Equals(Right, ESearchCase::IgnoreCase)
		|| FPaths::IsUnderDirectory(Left, Right) || FPaths::IsUnderDirectory(Right, Left);
}
}

bool IsHexIdentity(const FString& Value, int32 Length)
{
	if (Value.Len() != Length) { return false; }
	for (TCHAR Character : Value)
	{
		if (!((Character >= TEXT('0') && Character <= TEXT('9'))
			|| (Character >= TEXT('a') && Character <= TEXT('f'))
			|| (Character >= TEXT('A') && Character <= TEXT('F')))) { return false; }
	}
	return true;
}

FString BuildConfiguration()
{
#if UE_BUILD_SHIPPING
	return TEXT("Shipping");
#elif UE_BUILD_DEVELOPMENT
	return TEXT("Development");
#elif UE_BUILD_TEST
	return TEXT("Test");
#else
	return TEXT("Debug");
#endif
}

bool IsCookedGameProcess()
{
#if WITH_EDITOR || !UE_GAME
	return false;
#else
	return FPlatformProperties::RequiresCookedData() && FApp::IsGame() && !GIsEditor && !IsRunningCommandlet();
#endif
}

bool FUiSavePaths::Initialize(FString& Error)
{
	FParse::Value(FCommandLine::Get(), TEXT("UserDir="), UserDir);
	FParse::Value(FCommandLine::Get(), TEXT("AvidScriptUiSaveReport="), ReportPath);
	if (!PathPrivate::IsAbsoluteLocalPath(UserDir) || !PathPrivate::IsAbsoluteLocalPath(ReportPath))
	{
		Error = TEXT("absolute_local_UserDir_and_report_required"); return false;
	}
	UserDir = PathPrivate::FullPath(UserDir);
	ReportPath = PathPrivate::FullPath(ReportPath);
	EffectiveSavedDir = PathPrivate::FullPath(FPaths::ProjectSavedDir());
	const FString ExpectedSaved = PathPrivate::FullPath(UserDir / TEXT("Saved"));
	const FString PackageRoot = PathPrivate::FullPath(FPaths::RootDir());
	// BuildCookRun archives Windows packages in <archive>/Windows.
	const FString ArchiveRoot = FPaths::GetCleanFilename(PackageRoot).Equals(TEXT("Windows"), ESearchCase::IgnoreCase)
		? FPaths::GetPath(PackageRoot) : PackageRoot;
	for (const FString& Forbidden : { PathPrivate::FullPath(FPaths::ProjectDir()),
		PathPrivate::FullPath(FPaths::EngineDir()), PackageRoot, ArchiveRoot })
	{
		if (PathPrivate::Overlaps(UserDir, Forbidden)) { Error = TEXT("UserDir_overlaps_project_engine_or_archive"); return false; }
	}
	if (!EffectiveSavedDir.Equals(ExpectedSaved, ESearchCase::IgnoreCase))
	{
		Error = TEXT("UserDir_does_not_isolate_effective_Saved_directory"); return false;
	}
	SavePath = ExpectedSaved / TEXT("SaveGames/AvidScript_UiSaveDemo_v1.sav");
	const FString SaveDirectory = FPaths::GetPath(SavePath);
	IPlatformFile& Files = IPlatformFile::GetPlatformPhysical();
	if (!FPaths::IsUnderDirectory(ReportPath, ExpectedSaved)
		|| PathPrivate::Overlaps(ReportPath, SaveDirectory)
		|| !FPaths::GetExtension(ReportPath).Equals(TEXT("json"), ESearchCase::IgnoreCase)
		|| Files.FileExists(*ReportPath) || Files.DirectoryExists(*ReportPath)
		|| !PathPrivate::HasSafeParents(ReportPath) || !PathPrivate::HasSafeParents(SavePath))
	{
		Error = TEXT("report_requires_safe_unique_new_json_under_UserDir_Saved"); return false;
	}
	bInitialized = true;
	return CheckSaveDirectory(Error);
}

bool FUiSavePaths::CheckSaveDirectory(FString& Error) const
{
	if (!bInitialized || !PathPrivate::HasSafeParents(SavePath) || !PathPrivate::HasSafeParents(ReportPath)
		|| !PathPrivate::FullPath(FPaths::ProjectSavedDir()).Equals(EffectiveSavedDir, ESearchCase::IgnoreCase))
	{
		Error = TEXT("unsafe_or_changed_save_report_path"); return false;
	}
	IPlatformFile& Files = IPlatformFile::GetPlatformPhysical();
	const FString Directory = FPaths::GetPath(SavePath);
	if (Files.FileExists(*Directory)) { Error = TEXT("save_directory_is_a_file"); return false; }
	if (!Files.DirectoryExists(*Directory)) { return true; }
	bool bSafe = true;
	// Inspect only this isolated directory's entries. Never recurse or read another save.
	const bool bVisited = Files.IterateDirectory(*Directory, [this, &bSafe](const TCHAR* Filename, bool bDirectory)
	{
		bSafe = !bDirectory && PathPrivate::FullPath(Filename).Equals(SavePath, ESearchCase::IgnoreCase)
			&& PathPrivate::HasSafeParents(Filename);
		return bSafe;
	});
	if (!bVisited || !bSafe) { Error = TEXT("isolated_save_directory_contains_other_entries"); return false; }
	return true;
}

bool FUiSavePaths::ReadSave(FString& Error)
{
	if (!CheckSaveDirectory(Error)) { return false; }
	IPlatformFile& Files = IPlatformFile::GetPlatformPhysical();
	bSaveExists = Files.FileExists(*SavePath);
	SaveHash.Reset();
	SaveBytes = 0;
	if (!bSaveExists) { return true; }
	TUniquePtr<IFileHandle> File(Files.OpenRead(*SavePath));
	if (!File) { Error = TEXT("save_read_failed"); return false; }
	SaveBytes = File->Size();
	if (SaveBytes < 0 || SaveBytes > 16 * 1024 * 1024) { Error = TEXT("save_size_invalid"); return false; }
	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(static_cast<int32>(SaveBytes));
	if (SaveBytes > 0 && !File->Read(Bytes.GetData(), SaveBytes)) { Error = TEXT("save_read_failed"); return false; }
	SaveHash = FAvidScriptHash::Sha256Hex(Bytes);
	return true;
}

bool FUiSavePaths::WriteNewReport(const FString& Json, FString& Error) const
{
	// A failed save-directory check must not suppress an otherwise safely confined failure report.
	if (!bInitialized || !PathPrivate::HasSafeParents(ReportPath)
		|| !PathPrivate::FullPath(FPaths::ProjectSavedDir()).Equals(EffectiveSavedDir, ESearchCase::IgnoreCase))
	{
		Error = TEXT("report_path_not_authorized"); return false;
	}
	IPlatformFile& Files = IPlatformFile::GetPlatformPhysical();
	if (Files.FileExists(*ReportPath) || Files.DirectoryExists(*ReportPath)
		|| !Files.CreateDirectoryTree(*FPaths::GetPath(ReportPath)) || !PathPrivate::HasSafeParents(ReportPath))
	{
		Error = TEXT("report_requires_new_safe_path"); return false;
	}
	TUniquePtr<FArchive> Writer(IFileManager::Get().CreateFileWriter(*ReportPath, FILEWRITE_NoReplaceExisting));
	if (!Writer) { Error = TEXT("report_create_failed"); return false; }
	FTCHARToUTF8 Utf8(*Json);
	Writer->Serialize(const_cast<ANSICHAR*>(Utf8.Get()), Utf8.Length());
	const bool bClosed = Writer->Close();
	if (!bClosed || Writer->IsError()) { Error = TEXT("report_write_failed"); return false; }
	return true;
}
}
