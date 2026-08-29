#pragma once

#include "CoreMinimal.h"

class FAvidScriptArrayValueHeap;
class FAvidScriptBindingPackage;
class FAvidScriptObjectRegistry;
class FAvidScriptUtf8ValueHeap;
class IAvidScriptObjectOwnershipDomain;
struct FAvidScriptBindingLatentCompletionPayload;
struct FAvidScriptBindingTypeModel;

struct FAvidScriptContinuationResultCodecTransaction
{
	TArray<uint32, TInlineAllocator<2>> CreatedUtf8Tokens;
	TArray<uint32, TInlineAllocator<2>> CreatedArrayTokens;

	void Commit();
	void Rollback(
		FAvidScriptUtf8ValueHeap& Utf8ValueHeap,
		FAvidScriptArrayValueHeap& ArrayValueHeap);
};

class FAvidScriptContinuationResultCodec
{
public:
	static bool Encode(
		const FAvidScriptBindingLatentCompletionPayload& Payload,
		const FAvidScriptBindingTypeModel& Type,
		const FAvidScriptBindingPackage& Package,
		FAvidScriptObjectRegistry* ObjectRegistry,
		IAvidScriptObjectOwnershipDomain* ObjectOwnership,
		FAvidScriptUtf8ValueHeap& Utf8ValueHeap,
		FAvidScriptArrayValueHeap& ArrayValueHeap,
		TArrayView<uint8> OutBytes,
		FAvidScriptContinuationResultCodecTransaction& Transaction,
		FString& OutError);
};
