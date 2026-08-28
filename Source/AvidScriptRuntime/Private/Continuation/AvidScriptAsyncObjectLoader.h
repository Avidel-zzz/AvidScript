#pragma once

#include "CoreMinimal.h"

struct FSoftObjectPath;

class IAvidScriptAsyncObjectLoadHandle
{
public:
	virtual ~IAvidScriptAsyncObjectLoadHandle() = default;

	virtual void Cancel() = 0;
};

class IAvidScriptAsyncObjectLoader
{
public:
	using FCompletion = TFunction<void(UObject*)>;

	virtual ~IAvidScriptAsyncObjectLoader() = default;

	virtual TSharedPtr<IAvidScriptAsyncObjectLoadHandle> RequestAsyncLoad(
		const FSoftObjectPath& ObjectPath,
		FCompletion&& Completion) = 0;
};

TSharedRef<IAvidScriptAsyncObjectLoader> CreateAvidScriptAsyncObjectLoader();
