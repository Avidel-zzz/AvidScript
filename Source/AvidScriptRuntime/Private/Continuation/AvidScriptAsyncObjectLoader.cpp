#include "Continuation/AvidScriptAsyncObjectLoader.h"

#include "Engine/StreamableManager.h"
#include "UObject/SoftObjectPath.h"

namespace
{
struct FAvidScriptStreamableLoadState
{
	void Complete()
	{
		if (bCancelled || bCompleted)
		{
			return;
		}

		bCompleted = true;
		bCompleting = true;
		UObject* const LoadedObject = Handle
			? Handle->GetLoadedAsset()
			: ObjectPath.ResolveObject();
		IAvidScriptAsyncObjectLoader::FCompletion LocalCompletion =
			MoveTemp(Completion);
		if (LocalCompletion)
		{
			LocalCompletion(LoadedObject);
		}
		bCompleting = false;
		ReleaseProducerHandle();
	}

	void Cancel()
	{
		if (!bCancelled && !bCompleted)
		{
			bCancelled = true;
			Completion.Reset();
			if (Handle)
			{
				Handle->CancelHandle();
			}
		}
		if (bCompleting)
		{
			return;
		}
		ReleaseProducerHandle();
	}

	void ReleaseProducerHandle()
	{
		if (!bProducerHandleReleased)
		{
			bProducerHandleReleased = true;
			Handle.Reset();
		}
	}

	bool AttachProducerHandle(TSharedPtr<FStreamableHandle> InHandle)
	{
		if (!InHandle)
		{
			return false;
		}
		if (bCompleted || bProducerHandleReleased)
		{
			InHandle.Reset();
			return true;
		}
		if (bCancelled)
		{
			InHandle->CancelHandle();
			InHandle.Reset();
			bProducerHandleReleased = true;
			return false;
		}

		Handle = MoveTemp(InHandle);
		return true;
	}

	FSoftObjectPath ObjectPath;
	IAvidScriptAsyncObjectLoader::FCompletion Completion;
	TSharedPtr<FStreamableHandle> Handle;
	bool bCancelled = false;
	bool bCompleted = false;
	bool bCompleting = false;
	bool bProducerHandleReleased = false;
};

class FAvidScriptStreamableLoadHandle final
	: public IAvidScriptAsyncObjectLoadHandle
{
public:
	explicit FAvidScriptStreamableLoadHandle(
		TSharedRef<FAvidScriptStreamableLoadState> InState)
		: State(MoveTemp(InState))
	{
	}

	virtual ~FAvidScriptStreamableLoadHandle() override
	{
		State->Cancel();
	}

	virtual void Cancel() override
	{
		State->Cancel();
	}

private:
	TSharedRef<FAvidScriptStreamableLoadState> State;
};

class FAvidScriptAsyncObjectLoader final : public IAvidScriptAsyncObjectLoader
{
public:
	virtual TSharedPtr<IAvidScriptAsyncObjectLoadHandle> RequestAsyncLoad(
		const FSoftObjectPath& ObjectPath,
		FCompletion&& Completion) override
	{
		TSharedRef<FAvidScriptStreamableLoadState> State =
			MakeShared<FAvidScriptStreamableLoadState>();
		State->ObjectPath = ObjectPath;
		State->Completion = MoveTemp(Completion);
		TSharedPtr<FStreamableHandle> ProducerHandle =
			StreamableManager.RequestAsyncLoad(
			ObjectPath,
			FStreamableDelegate::CreateLambda([State]()
			{
				State->Complete();
			}));
		if (!State->AttachProducerHandle(MoveTemp(ProducerHandle)))
		{
			State->Cancel();
			return nullptr;
		}

		return MakeShared<FAvidScriptStreamableLoadHandle>(MoveTemp(State));
	}

private:
	FStreamableManager StreamableManager;
};
} // namespace

TSharedRef<IAvidScriptAsyncObjectLoader> CreateAvidScriptAsyncObjectLoader()
{
	return MakeShared<FAvidScriptAsyncObjectLoader>();
}
