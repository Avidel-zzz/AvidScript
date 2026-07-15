#pragma once

#include "CoreMinimal.h"

class AVIDSCRIPTCORE_API FAvidScriptHash
{
public:
	static FString Sha256Hex(TConstArrayView<uint8> Bytes);
	static FString Sha256HexUtf8(const FString& Value);
};
