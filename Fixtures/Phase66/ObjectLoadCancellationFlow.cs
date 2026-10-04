using System;
using System.Threading;
using System.Threading.Tasks;
using AvidScript;

// These method bodies are compiled unchanged by .NET and the production guest build.
public static class ObjectCancellationScript
{
    public static int Mode;
    public static int Result;
    public static int Trace;
    public static int LoadedSlot;
    public static int LoadedGeneration;
    public static int InnerCatch;
    public static int OuterCatch;
    public static int WrongCatch;
    public static int IdentityMatch;
    public static AvidCancellationSource Lifetime;

    public static async Task<int> LoadAsync(CancellationToken token)
    {
        try
        {
            var loaded = await AvidAssets.LoadObjectAsync(
                "/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture").WithCancellation(token);
            LoadedSlot = loaded.Slot;
            LoadedGeneration = loaded.Generation;
            Trace = Trace * 10 + 2;
            if (Mode == 1) await AvidContinuations.NextTickAsync();
            return loaded.Slot == 0 ? 20 : 10;
        }
        catch (TaskCanceledException error)
        {
            InnerCatch++;
            IdentityMatch = error.CancellationToken == token ? 1 : 0;
            if (Mode == 2) throw;
            return 30;
        }
        catch (OperationCanceledException)
        {
            WrongCatch++;
            return 99;
        }
        finally { Trace = Trace * 10 + 3; }
    }

    public static async Task<int> RunAsync()
    {
        Lifetime = AvidCancellationSource.Create();
        CancellationToken token = Lifetime.Token;
        if (Mode == 3) Lifetime.Cancel();
        try
        {
            int value = await LoadAsync(token);
            return value + ObjectCancellationCache.Value;
        }
        catch (OperationCanceledException error)
        {
            OuterCatch++;
            IdentityMatch = error.CancellationToken == token ? 1 : 0;
            return 40;
        }
        finally { Trace = Trace * 10 + 4; Lifetime.Release(); }
    }
}

public static class ObjectCancellationCache
{
    public static int Value;
    static ObjectCancellationCache() { Value = 7; }
}
