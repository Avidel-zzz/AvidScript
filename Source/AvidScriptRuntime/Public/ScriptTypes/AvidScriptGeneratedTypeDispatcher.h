#pragma once

#include "CoreMinimal.h"

struct FAvidScriptGeneratedCallArgument
{
	void* Data = nullptr;
};

class IAvidScriptGeneratedTypeDispatchTarget
{
public:
	virtual ~IAvidScriptGeneratedTypeDispatchTarget() = default;

	virtual bool InvokeGeneratedTypeMember(
		UObject& Receiver,
		uint32 TypeOrdinal,
		uint32 MemberOrdinal,
		TConstArrayView<FAvidScriptGeneratedCallArgument> Arguments,
		void* Result) = 0;
};

class AVIDSCRIPTRUNTIME_API FAvidScriptGeneratedTypeDispatcher
{
public:
	static bool Install(IAvidScriptGeneratedTypeDispatchTarget& Target);
	// Drains in-flight dispatches; the target must not uninstall itself from its callback.
	static void Uninstall(IAvidScriptGeneratedTypeDispatchTarget& Target);

	static bool Invoke(
		UObject* Receiver,
		uint32 TypeOrdinal,
		uint32 MemberOrdinal,
		TConstArrayView<FAvidScriptGeneratedCallArgument> Arguments,
		void* Result);
};
