#pragma once

#include "AvidScriptObjectRegistry.h"
#include "ScriptTypes/AvidScriptGeneratedTypeDispatcher.h"
#include "UObject/ObjectKey.h"

class FAvidScriptGeneratedTypeRouter;

class AVIDSCRIPTRUNTIME_API IAvidScriptGeneratedTypeInstance
{
public:
	virtual ~IAvidScriptGeneratedTypeInstance() = default;

	virtual bool InvokeGeneratedTypeMember(
		UObject& Receiver,
		const FAvidScriptObjectHandle& ReceiverHandle,
		uint32 TypeOrdinal,
		uint32 MemberOrdinal,
		TConstArrayView<FAvidScriptGeneratedCallArgument> Arguments,
		void* Result) = 0;
};

class AVIDSCRIPTRUNTIME_API FAvidScriptGeneratedTypeInstanceRegistration
{
public:
	FAvidScriptGeneratedTypeInstanceRegistration() = default;
	~FAvidScriptGeneratedTypeInstanceRegistration();
	FAvidScriptGeneratedTypeInstanceRegistration(
		FAvidScriptGeneratedTypeInstanceRegistration&& Other);
	FAvidScriptGeneratedTypeInstanceRegistration& operator=(
		FAvidScriptGeneratedTypeInstanceRegistration&& Other);

	FAvidScriptGeneratedTypeInstanceRegistration(
		const FAvidScriptGeneratedTypeInstanceRegistration&) = delete;
	FAvidScriptGeneratedTypeInstanceRegistration& operator=(
		const FAvidScriptGeneratedTypeInstanceRegistration&) = delete;

	bool IsValid() const;
	bool Reset();

private:
	friend class FAvidScriptGeneratedTypeRouter;

	void Invalidate();
	void MoveFrom(FAvidScriptGeneratedTypeInstanceRegistration& Other);

	FObjectKey ReceiverKey;
	IAvidScriptGeneratedTypeInstance* Instance = nullptr;
	uint64 Serial = 0;
};

class AVIDSCRIPTRUNTIME_API FAvidScriptGeneratedTypeRouter final
	: public IAvidScriptGeneratedTypeDispatchTarget
{
public:
	static FAvidScriptGeneratedTypeRouter& Get();

	bool Startup();
	void Shutdown();
	bool RegisterInstance(
		UObject& Receiver,
		const FAvidScriptObjectHandle& ReceiverHandle,
		IAvidScriptGeneratedTypeInstance& Instance,
		FAvidScriptGeneratedTypeInstanceRegistration& OutRegistration);
	bool UnregisterInstance(FAvidScriptGeneratedTypeInstanceRegistration& Registration);

	bool InvokeGeneratedTypeMember(
		UObject& Receiver,
		uint32 TypeOrdinal,
		uint32 MemberOrdinal,
		TConstArrayView<FAvidScriptGeneratedCallArgument> Arguments,
		void* Result) override;

#if WITH_DEV_AUTOMATION_TESTS
	int32 GetRegisteredInstanceCountForTesting() const;
#endif

private:
	struct FImpl;

	FAvidScriptGeneratedTypeRouter();
	~FAvidScriptGeneratedTypeRouter();
	FAvidScriptGeneratedTypeRouter(const FAvidScriptGeneratedTypeRouter&) = delete;
	FAvidScriptGeneratedTypeRouter& operator=(const FAvidScriptGeneratedTypeRouter&) = delete;

	TUniquePtr<FImpl> Impl;
};
