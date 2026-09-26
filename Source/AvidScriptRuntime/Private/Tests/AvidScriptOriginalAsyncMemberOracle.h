#pragma once

#include "AvidScriptCancellationTaskObserver.h"
#include "AvidScriptWasmRuntime.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"

namespace AvidScript::Tests::OriginalAsyncMember
{
struct FTaskExpectation
{
    FString Name;
    int32 CallbackId = 0;
    EAvidScriptTaskResultState Initial = EAvidScriptTaskResultState::Running;
    EAvidScriptTaskResultState Terminal = EAvidScriptTaskResultState::Running;
    int32 Result = 0;
    TSet<int32> ErrorTypes;
};

struct FFieldExpectation
{
    FString Name;
    int32 Offset = -1;
    int32 Value = 0;
};

// Test manifest only. Callback IDs come from the validated source await routes,
// not from Task allocation order or a count of compiler-internal error owners.
struct FOracle
{
    TArray<FTaskExpectation> Tasks;
    TSet<int32> LoopCallbacks;
    TArray<TArray<FFieldExpectation>> LoopSuspensions;
    int32 CheckedSuspensions = 0;

    static bool ReadState(const FString& Name, EAvidScriptTaskResultState& Out)
    {
        if (Name == TEXT("running")) Out = EAvidScriptTaskResultState::Running;
        else if (Name == TEXT("succeeded")) Out = EAvidScriptTaskResultState::Succeeded;
        else if (Name == TEXT("faulted")) Out = EAvidScriptTaskResultState::Faulted;
        else if (Name == TEXT("cancelled")) Out = EAvidScriptTaskResultState::Cancelled;
        else return false;
        return true;
    }

    bool Read(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Scenario, const FString& Name)
    {
        const TArray<TSharedPtr<FJsonValue>> *TaskEntries = nullptr, *Callbacks = nullptr, *Suspensions = nullptr;
        const bool bLoop = Name.EndsWith(TEXT("-loop"));
        if (!Test.TestTrue(TEXT("Original source Task and loop observations are complete"),
            Scenario->TryGetArrayField(TEXT("taskObservations"), TaskEntries) && TaskEntries
            && TaskEntries->Num() == (Name.EndsWith(TEXT("-concurrent")) ? 2 : 1)
            && Scenario->TryGetArrayField(TEXT("loopCallbacks"), Callbacks) && Callbacks
            && Callbacks->Num() == (bLoop ? 2 : 0)
            && Scenario->TryGetArrayField(TEXT("loopSuspensions"), Suspensions) && Suspensions
            && Suspensions->Num() == (bLoop ? 2 : 0))) return false;
        TSet<int32> TaskCallbacks;
        TSet<FString> TaskNames;
        for (const auto& Value : *TaskEntries)
        {
            const TSharedPtr<FJsonObject>* Entry = nullptr;
            FTaskExpectation Task;
            FString Initial, Terminal;
            const TArray<TSharedPtr<FJsonValue>>* ErrorTypes = nullptr;
            if (!Test.TestTrue(TEXT("Source Task observation is bounded and unique"), Value && Value->TryGetObject(Entry)
                && Entry && Entry->IsValid() && (*Entry)->TryGetStringField(TEXT("name"), Task.Name)
                && (Task.Name == TEXT("pending") || Task.Name == TEXT("second")) && !TaskNames.Contains(Task.Name)
                && (*Entry)->TryGetNumberField(TEXT("callbackId"), Task.CallbackId) && Task.CallbackId > 0
                && !TaskCallbacks.Contains(Task.CallbackId)
                && (*Entry)->TryGetStringField(TEXT("initialState"), Initial) && ReadState(Initial, Task.Initial)
                && (*Entry)->TryGetStringField(TEXT("terminalState"), Terminal) && ReadState(Terminal, Task.Terminal)
                && Task.Terminal != EAvidScriptTaskResultState::Running
                && (*Entry)->TryGetNumberField(TEXT("result"), Task.Result)
                && (*Entry)->TryGetArrayField(TEXT("errorTypeTokens"), ErrorTypes) && ErrorTypes
                && (Task.Terminal == EAvidScriptTaskResultState::Succeeded ? ErrorTypes->IsEmpty()
                    : !ErrorTypes->IsEmpty() && ErrorTypes->Num() <= 8))) return false;
            for (const auto& Type : *ErrorTypes)
            {
                int32 Token = 0;
                if (!Test.TestTrue(TEXT("Exception type token is positive and unique"), Type && Type->TryGetNumber(Token)
                    && Token > 0 && !Task.ErrorTypes.Contains(Token))) return false;
                Task.ErrorTypes.Add(Token);
            }
            TaskCallbacks.Add(Task.CallbackId);
            TaskNames.Add(Task.Name);
            Tasks.Add(MoveTemp(Task));
        }
        for (const auto& Value : *Callbacks)
        {
            int32 Callback = 0;
            if (!Test.TestTrue(TEXT("Loop callback is distinct from source Task observations"), Value
                && Value->TryGetNumber(Callback) && Callback > 0 && !LoopCallbacks.Contains(Callback)
                && !TaskCallbacks.Contains(Callback))) return false;
            LoopCallbacks.Add(Callback);
        }
        for (const auto& Value : *Suspensions)
        {
            const TArray<TSharedPtr<FJsonValue>>* Fields = nullptr;
            if (!Test.TestTrue(TEXT("Loop suspension observes all 13 business fields"), Value
                && Value->TryGetArray(Fields) && Fields && Fields->Num() == 13)) return false;
            TArray<FFieldExpectation> ExpectedFields;
            TSet<FString> Names;
            for (const auto& Field : *Fields)
            {
                const TSharedPtr<FJsonObject>* Entry = nullptr;
                FFieldExpectation Expected;
                if (!Test.TestTrue(TEXT("Loop field name and address are bounded and unique"), Field && Field->TryGetObject(Entry)
                    && Entry && Entry->IsValid() && (*Entry)->TryGetStringField(TEXT("name"), Expected.Name)
                    && !Expected.Name.IsEmpty() && !Names.Contains(Expected.Name)
                    && (*Entry)->TryGetNumberField(TEXT("offset"), Expected.Offset) && Expected.Offset >= 0 && Expected.Offset < 65536
                    && (*Entry)->TryGetNumberField(TEXT("expected"), Expected.Value))) return false;
                Names.Add(Expected.Name);
                ExpectedFields.Add(MoveTemp(Expected));
            }
            LoopSuspensions.Add(MoveTemp(ExpectedFields));
        }
        return true;
    }

    bool CheckSuspension(FAutomationTestBase& Test, const FString& Label,
        const CompiledCancellation::FTaskObserver& Observer, FAvidScriptWasmRuntimeInstance& Runtime)
    {
        int32 Suspensions = 0;
        for (const auto& Await : Observer.AwaitRegistrations)
        {
            if (!LoopCallbacks.Contains(Await.CallbackId)) continue;
            if (!Test.TestTrue(*(Label + TEXT(" loop awaits pending producer")),
                Await.Registration == EAvidScriptTaskWaitRegistration::Queued)) return false;
            ++Suspensions;
        }
        if (Suspensions == CheckedSuspensions) return true;
        if (!Test.TestTrue(*(Label + TEXT(" next loop suspension reached exactly once")),
            Suspensions == CheckedSuspensions + 1 && Suspensions <= LoopSuspensions.Num())) return false;
        // Called after a VM invocation returns, never reentrantly from a host
        // import. Tick only copies business values to test adapter int fields.
        FAvidScriptWasmSmokeResult Result;
        if (!Test.TestTrue(*(Label + TEXT(" snapshot at loop suspension")), Runtime.Tick(0.0f, Result)))
        { Test.AddError(Result.ErrorMessage); return false; }
        for (const auto& Field : LoopSuspensions[CheckedSuspensions])
        {
            uint8 Bytes[4] = {};
            FString Error;
            if (!Runtime.ReadStateBytes(Field.Offset, MakeArrayView(Bytes), Error))
            { Test.AddError(Error); return false; }
            int32 Value = 0;
            FMemory::Memcpy(&Value, Bytes, sizeof(Value));
            Test.TestEqual(*FString::Printf(TEXT("%s loop suspension=%d %s"), *Label, Suspensions, *Field.Name), Value, Field.Value);
        }
        ++CheckedSuspensions;
        return true;
    }

    bool CheckFinal(FAutomationTestBase& Test, const FString& Label, const CompiledCancellation::FTaskObserver& Observer) const
    {
        Test.TestEqual(*(Label + TEXT(" observed loop suspension count")), CheckedSuspensions, LoopSuspensions.Num());
        Test.TestTrue(*(Label + TEXT(" immutable Task terminal snapshots")), Observer.bStableTerminalReads);
        TSet<int64> SeenTasks;
        for (const auto& Expected : Tasks)
        {
            const auto Matches = Observer.AwaitRegistrations.FilterByPredicate([&](const auto& Await) {
                return Await.CallbackId == Expected.CallbackId;
            });
            if (!Test.TestEqual(*(Label + TEXT(" source await ") + Expected.Name), Matches.Num(), 1)) return false;
            const int64 Token = Matches[0].Token;
            if (!Test.TestTrue(*(Label + TEXT(" independent source Task ") + Expected.Name), !SeenTasks.Contains(Token))) return false;
            SeenTasks.Add(Token);
            const auto* Initial = Observer.EntryStates.Find(Token);
            const auto* Terminal = Observer.TerminalSnapshots.Find(Token);
            if (!Test.TestTrue(*(Label + TEXT(" Task snapshots exist ") + Expected.Name), Initial && Terminal)) return false;
            Test.TestTrue(*(Label + TEXT(" Task entry state ") + Expected.Name), *Initial == Expected.Initial);
            Test.TestTrue(*(Label + TEXT(" Task terminal state ") + Expected.Name), Terminal->State == Expected.Terminal);
            Test.TestEqual(*(Label + TEXT(" Task value codec")), Terminal->TypeId, FString(TEXT("type:int32")));
            Test.TestEqual(*(Label + TEXT(" expected language failure marker")), Terminal->ErrorCode,
                Expected.Terminal == EAvidScriptTaskResultState::Faulted ? FString(TEXT("language_error")) : FString());
            if (Expected.Terminal == EAvidScriptTaskResultState::Succeeded)
            {
                if (!Test.TestEqual(*(Label + TEXT(" Task value width")), Terminal->Value.Num(), 4)) return false;
                int32 Value = 0;
                FMemory::Memcpy(&Value, Terminal->Value.GetData(), sizeof(Value));
                Test.TestEqual(*(Label + TEXT(" Task value ") + Expected.Name), Value, Expected.Result);
                Test.TestFalse(*(Label + TEXT(" success has no exception")), Terminal->LanguageError.IsSet());
            }
            else
            {
                Test.TestEqual(*(Label + TEXT(" failed Task has no value")), Terminal->Value.Num(), 0);
                if (!Test.TestTrue(*(Label + TEXT(" Task preserves language exception")), Terminal->LanguageError.IsSet())) return false;
                Test.TestTrue(*(Label + TEXT(" exception matches reference catch type")),
                    Expected.ErrorTypes.Contains(Terminal->LanguageError->TypeToken));
                Test.TestTrue(*(Label + TEXT(" exception carries source and object identity")),
                    Terminal->LanguageError->SourceToken > 0 && Terminal->LanguageError->ObjectToken != 0);
            }
        }
        Test.AddInfo(FString::Printf(TEXT("original-task-oracle %s tasks=%d loop-suspensions=%d"),
            *Label, Tasks.Num(), CheckedSuspensions));
        return true;
    }
};
}
