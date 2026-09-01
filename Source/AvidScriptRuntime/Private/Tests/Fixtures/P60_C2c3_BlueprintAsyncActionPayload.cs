using System.Runtime.InteropServices;

namespace AvidScript;

public static class P60C2c3BlueprintAsyncActionPayload
{
    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static async void BeginPlay()
    {
        UE.Self.SetActorScale3D(new FVector(1.0f, 1.0f, 1.0f));

        AvidOutcome<AvidScriptEditorAsyncActionPayloadTestObject_WaitForPayloadResult> result =
            await UAvidScriptEditorAsyncActionPayloadTestObject.WaitForPayloadAsync();
        float statusState = 9.0f;
        if (result.Succeeded)
        {
            statusState = 1.0f;
        }
        if (result.Failed)
        {
            statusState = 2.0f;
        }
        if (result.Cancelled)
        {
            statusState = 3.0f;
        }

        float outcomeState = 9.0f;
        if (result.Value.Outcome ==
            AvidScriptEditorAsyncActionPayloadTestObject_WaitForPayloadOutcome.Completed)
        {
            outcomeState = 0.0f;
        }
        if (result.Value.Outcome ==
            AvidScriptEditorAsyncActionPayloadTestObject_WaitForPayloadOutcome.Failed)
        {
            outcomeState = 1.0f;
        }

        float scoreState = (float)result.Value.Completed_Score;
        if (!result.Value.Completed_Target.HasHandle)
        {
            scoreState = -scoreState;
        }

        UE.Self.SetActorScale3D(new FVector(
            scoreState,
            statusState,
            outcomeState));
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds)
    {
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
    public static void EndPlay()
    {
    }
}
