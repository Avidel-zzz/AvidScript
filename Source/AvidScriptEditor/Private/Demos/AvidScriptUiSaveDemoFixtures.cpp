#include "Demos/AvidScriptUiSaveDemoFixtures.h"

#include "AvidScriptHash.h"
#include "Demos/AvidScriptUiSaveDemoProbeTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/PlatformFileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace AvidScript::UiSaveDemo
{
namespace
{
constexpr int64 MaximumFixtureBytes = 4 * 1024 * 1024;

bool ReadFixtureBytes(IFileHandle& File, TArray<uint8>& Bytes, FString& Error)
{
	Bytes.Reset();
	const int64 Size = File.Size();
	if (Size < 0 || Size > MaximumFixtureBytes || !File.Seek(0))
	{
		Error = TEXT("fixture_read_size_or_seek_failed");
		return false;
	}
	Bytes.SetNumUninitialized(static_cast<int32>(Size));
	if (Size > 0 && !File.Read(Bytes.GetData(), Size))
	{
		Bytes.Reset();
		Error = TEXT("fixture_read_failed");
		return false;
	}
	return true;
}
}

FSaveFixtureController::FSaveFixtureController(
	const FString& InSavePath, TFunction<bool()> InCheckSafe)
	: SavePath(InSavePath)
	, CheckSafe(MoveTemp(InCheckSafe))
{
}

FSaveFixtureController::~FSaveFixtureController()
{
	Unlock();
}

bool FSaveFixtureController::CheckPath(FString& Error) const
{
	if (SavePath.IsEmpty() || FPaths::IsRelative(SavePath) || !CheckSafe || !CheckSafe())
	{
		Error = TEXT("fixture_path_unsafe");
		return false;
	}
	return true;
}

bool FSaveFixtureController::ReadCurrent(TArray<uint8>& Bytes, FString& Error) const
{
	Bytes.Reset();
	if (!CheckPath(Error))
	{
		return false;
	}
	TUniquePtr<IFileHandle> File(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*SavePath, false));
	if (!File)
	{
		Error = TEXT("fixture_open_read_failed");
		return false;
	}
	return ReadFixtureBytes(*File, Bytes, Error);
}

bool FSaveFixtureController::ObserveScriptSave(FString& Error)
{
	Error.Reset();
	if (!ExpectedHash.IsEmpty())
	{
		Error = TEXT("fixture_owner_already_observed");
		return false;
	}
	TArray<uint8> Bytes;
	if (!ReadCurrent(Bytes, Error))
	{
		return false;
	}
	if (Bytes.IsEmpty())
	{
		Error = TEXT("fixture_script_save_empty");
		return false;
	}
	ExpectedHash = FAvidScriptHash::Sha256Hex(Bytes);
	return true;
}

bool FSaveFixtureController::CheckUnchanged(FString& Error) const
{
	Error.Reset();
	if (ExpectedHash.IsEmpty())
	{
		Error = TEXT("fixture_owner_not_observed");
		return false;
	}
	TArray<uint8> Bytes;
	if (!ReadCurrent(Bytes, Error))
	{
		return false;
	}
	if (FAvidScriptHash::Sha256Hex(Bytes) != ExpectedHash)
	{
		Error = TEXT("fixture_external_change_detected");
		return false;
	}
	return true;
}

bool FSaveFixtureController::Prepare(const FString& Kind, FString& Error)
{
	Error.Reset();
	if (!IsInGameThread())
	{
		Error = TEXT("fixture_prepare_requires_game_thread");
		return false;
	}
	if (IsLocked())
	{
		Error = TEXT("fixture_prepare_while_locked");
		return false;
	}
	static const TCHAR* const Kinds[] = {
		TEXT("wrong_type"), TEXT("negative"), TEXT("overflow"), TEXT("empty"), TEXT("valid")
	};
	if (Evidence.Num() >= UE_ARRAY_COUNT(Kinds) || Kind != Kinds[Evidence.Num()])
	{
		Error = TEXT("fixture_kind_out_of_sequence");
		return false;
	}
	if (!CheckUnchanged(Error))
	{
		return false;
	}

	TArray<uint8> Bytes;
	if (Kind != TEXT("empty"))
	{
		TStrongObjectPtr<USaveGame> Save;
		if (Kind == TEXT("wrong_type"))
		{
			Save.Reset(NewObject<UAvidScriptUiSaveOtherSaveGame>(GetTransientPackage()));
		}
		else
		{
			UClass* SaveClass = LoadClass<USaveGame>(nullptr,
				TEXT("/AvidScript/Demos/UiSave/BP_PlayerSave.BP_PlayerSave_C"));
			if (SaveClass == nullptr || SaveClass->HasAnyClassFlags(CLASS_Abstract))
			{
				Error = TEXT("fixture_player_save_class_unavailable");
				return false;
			}
			FIntProperty* Score = FindFProperty<FIntProperty>(SaveClass, TEXT("Score"));
			if (Score == nullptr || Score->ArrayDim != 1)
			{
				Error = TEXT("fixture_player_save_score_contract_invalid");
				return false;
			}
			Save.Reset(NewObject<USaveGame>(GetTransientPackage(), SaveClass));
			if (!Save.IsValid())
			{
				Error = TEXT("fixture_player_save_creation_failed");
				return false;
			}
			const int32 Value = Kind == TEXT("negative") ? -1 : Kind == TEXT("overflow") ? 1000000 : 1;
			Score->SetPropertyValue_InContainer(Save.Get(), Value);
		}
		if (!Save.IsValid() || !UGameplayStatics::SaveGameToMemory(Save.Get(), Bytes) || Bytes.IsEmpty())
		{
			Error = TEXT("fixture_serialization_failed");
			return false;
		}
	}

	if (!CheckUnchanged(Error))
	{
		return false;
	}
	const FString BeforeHash = ExpectedHash;
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	// Preserve the contents until the hash is checked again under a write-exclusive handle.
	TUniquePtr<IFileHandle> File(PlatformFile.OpenWrite(*SavePath, true, true));
	if (!File)
	{
		Error = TEXT("fixture_open_write_failed");
		return false;
	}
	TArray<uint8> BeforeBytes;
	if (!ReadFixtureBytes(*File, BeforeBytes, Error))
	{
		return false;
	}
	if (FAvidScriptHash::Sha256Hex(BeforeBytes) != BeforeHash)
	{
		Error = TEXT("fixture_external_change_detected");
		return false;
	}
	if (!CheckPath(Error))
	{
		return false;
	}
	if (!File->Seek(0) || !File->Truncate(0)
		|| (!Bytes.IsEmpty() && !File->Write(Bytes.GetData(), Bytes.Num()))
		|| !File->Flush(true))
	{
		Error = TEXT("fixture_write_failed");
		return false;
	}
	TArray<uint8> Readback;
	if (!ReadFixtureBytes(*File, Readback, Error))
	{
		return false;
	}
	if (Readback != Bytes)
	{
		Error = TEXT("fixture_write_readback_mismatch");
		return false;
	}
	File.Reset();
	if (!ReadCurrent(Readback, Error))
	{
		return false;
	}
	if (Readback != Bytes)
	{
		Error = TEXT("fixture_external_change_detected");
		return false;
	}

	ExpectedHash = FAvidScriptHash::Sha256Hex(Readback);
	TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
	Entry->SetStringField(TEXT("kind"), Kind);
	Entry->SetStringField(TEXT("before_sha256"), BeforeHash);
	Entry->SetStringField(TEXT("sha256"), ExpectedHash);
	Entry->SetNumberField(TEXT("bytes"), Readback.Num());
	Evidence.Add(MakeShared<FJsonValueObject>(Entry));
	return true;
}

bool FSaveFixtureController::LockForSaveFailure(FString& Error)
{
	Error.Reset();
	if (IsLocked())
	{
		return CheckUnchanged(Error);
	}
	if (!CheckUnchanged(Error))
	{
		return false;
	}
	ReadLock.Reset(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*SavePath, false));
	if (!ReadLock)
	{
		Error = TEXT("fixture_lock_failed");
		return false;
	}
	if (!CheckUnchanged(Error))
	{
		Unlock();
		return false;
	}
	return true;
}

void FSaveFixtureController::Unlock()
{
	ReadLock.Reset();
}

bool FSaveFixtureController::IsLocked() const
{
	return ReadLock.Get() != nullptr;
}

const FString& FSaveFixtureController::GetExpectedHash() const
{
	return ExpectedHash;
}

const TArray<TSharedPtr<FJsonValue>>& FSaveFixtureController::GetEvidence() const
{
	return Evidence;
}
}
