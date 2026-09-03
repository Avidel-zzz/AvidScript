using System.Runtime.InteropServices;
using AUiSaveHost = AvidScript.ABP_UiSaveHost_C;
using UUiSaveWidget = AvidScript.UWBP_UiSave_C;
using UPlayerSave = AvidScript.UBP_PlayerSave_C;

namespace AvidScript;

[AvidStateContract(AvidStateMode.Explicit)]
public static class UiSaveDemoScript
{
    public const string SaveSlot = "AvidScript_UiSaveDemo_v1";
    private const int UserIndex = 0;
    private const int MaximumScore = 999999;

    [AvidPersist]
    private static int Score;
    [AvidTransient]
    private static bool Ready;
    [AvidTransient]
    private static UPlayerSave OwnedSave;
    [AvidTransient]
    private static AvidSubscription CollectSubscription;
    [AvidTransient]
    private static AvidSubscription SaveSubscription;
    [AvidTransient]
    private static AvidSubscription LoadSubscription;
    [AvidTransient]
    private static AvidSubscription ResetSubscription;

    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static async void BeginPlay()
    {
        CancelSubscriptions();
        // UI writes run after activation commits; rejected candidates never resume.
        await AvidContinuations.NextTickAsync();
        UUiSaveWidget widget = GetWidget();
        if (!widget.HasHandle)
        {
            return;
        }

        UUserWidget userWidget = widget;
        UWidget baseWidget = userWidget;
        if (!baseWidget.IsInViewport())
        {
            userWidget.AddToViewport(0);
        }

        if (!widget.CollectButton.HasHandle || !widget.SaveButton.HasHandle
            || !widget.LoadButton.HasHandle || !widget.ResetButton.HasHandle
            || !widget.ScoreText.HasHandle || !widget.StatusText.HasHandle)
        {
            ShowStatus("UI unavailable");
            return;
        }

        CollectSubscription = AvidSubscriptions.SubscribeOnClicked(widget.CollectButton);
        SaveSubscription = AvidSubscriptions.SubscribeOnClicked(widget.SaveButton);
        LoadSubscription = AvidSubscriptions.SubscribeOnClicked(widget.LoadButton);
        ResetSubscription = AvidSubscriptions.SubscribeOnClicked(widget.ResetButton);
        Ready = CollectSubscription.IsValid && SaveSubscription.IsValid
            && LoadSubscription.IsValid && ResetSubscription.IsValid;
        if (!Ready)
        {
            CancelSubscriptions();
            ShowStatus("Input unavailable");
            return;
        }

        ShowScore();
        ShowStatus("Ready");
    }

    [AvidEvent(AvidEvents.OnClicked)]
    public static void OnClicked()
    {
        if (!Ready)
        {
            return;
        }
        UUiSaveWidget widget = GetWidget();
        if (!widget.HasHandle)
        {
            return;
        }

        if (AvidSubscriptions.IsCurrentSource(widget.CollectButton))
        {
            Collect();
        }
        else if (AvidSubscriptions.IsCurrentSource(widget.SaveButton))
        {
            Save();
        }
        else if (AvidSubscriptions.IsCurrentSource(widget.LoadButton))
        {
            Load();
        }
        else if (AvidSubscriptions.IsCurrentSource(widget.ResetButton))
        {
            Reset();
        }
    }

    private static void Collect()
    {
        if (Score >= MaximumScore)
        {
            ShowStatus("Score limit reached");
            return;
        }
        Score += 1;
        ShowScore();
        ShowStatus("Collected");
    }

    private static void Save()
    {
        AUiSaveHost host = UE.Self;
        if (!OwnedSave.HasHandle)
        {
            OwnedSave = UE.NewObject(host, ProjectFactories.PlayerSave);
        }
        UPlayerSave save = OwnedSave;
        if (!save.HasHandle)
        {
            ShowStatus("Save object unavailable");
            return;
        }

        // The host owns the GC-visible reference; the guest handle is not a strong reference.
        host.SavedObject = save;
        save.Score = Score;
        USaveGame saveGame = save;
        if (!UGameplayStatics.SaveGameToSlot(saveGame, SaveSlot, UserIndex))
        {
            ShowStatus("Save failed");
            return;
        }
        ShowStatus("Saved");
    }

    private static void Load()
    {
        if (!UGameplayStatics.DoesSaveGameExist(SaveSlot, UserIndex))
        {
            ShowStatus("No saved score");
            return;
        }

        AUiSaveHost host = UE.Self;
        USaveGame loaded = UGameplayStatics.LoadGameFromSlot(SaveSlot, UserIndex);
        if (!loaded.HasHandle)
        {
            ShowStatus("Load failed");
            return;
        }

        UPlayerSave save = UPlayerSave.TryCast(loaded);
        if (!save.HasHandle)
        {
            ShowStatus("Wrong save type");
            return;
        }
        int loadedScore = save.Score;
        if (loadedScore < 0 || loadedScore > MaximumScore)
        {
            ShowStatus("Invalid saved score");
            return;
        }

        host.SavedObject = save;
        Score = loadedScore;
        ShowScore();
        ShowStatus("Loaded");
    }

    private static void Reset()
    {
        Score = 0;
        ShowScore();
        ShowStatus("Reset");
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
    public static void EndPlay()
    {
        CancelSubscriptions();
        AUiSaveHost host = UE.Self;
        if (host.HasHandle)
        {
            UUiSaveWidget widget = GetWidget();
            if (widget.HasHandle)
            {
                UUserWidget userWidget = widget;
                UWidget baseWidget = userWidget;
                baseWidget.RemoveFromParent();
            }
            host.SavedObject = default;
        }
        UPlayerSave owned = OwnedSave;
        OwnedSave = default;
        if (owned.HasHandle)
        {
            UE.Release(owned);
        }
    }

    private static void CancelSubscriptions()
    {
        Ready = false;
        if (CollectSubscription.IsValid)
        {
            CollectSubscription.Cancel();
        }
        if (SaveSubscription.IsValid)
        {
            SaveSubscription.Cancel();
        }
        if (LoadSubscription.IsValid)
        {
            LoadSubscription.Cancel();
        }
        if (ResetSubscription.IsValid)
        {
            ResetSubscription.Cancel();
        }
        CollectSubscription = default;
        SaveSubscription = default;
        LoadSubscription = default;
        ResetSubscription = default;
    }

    private static UUiSaveWidget GetWidget()
    {
        AUiSaveHost host = UE.Self;
        if (!host.HasHandle)
        {
            return default;
        }
        UUserWidget widget = host.RootWidget;
        return UUiSaveWidget.TryCast(widget);
    }

    private static void ShowScore()
    {
        UUiSaveWidget widget = GetWidget();
        if (widget.HasHandle && widget.ScoreText.HasHandle)
        {
            FAvidText text = UKismetTextLibrary.Conv_IntToText(Score, false, false, 1, 6);
            widget.ScoreText.SetText(text);
            AvidScriptValue.Release(text);
        }
    }

    private static void ShowStatus(string message)
    {
        UUiSaveWidget widget = GetWidget();
        if (widget.HasHandle && widget.StatusText.HasHandle)
        {
            FAvidText text = UKismetTextLibrary.Conv_StringToText(message);
            widget.StatusText.SetText(text);
            AvidScriptValue.Release(text);
        }
    }
}
