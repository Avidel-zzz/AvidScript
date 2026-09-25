using System.Runtime.InteropServices;
using System.Threading.Tasks;
using AvidScript;

public static class Script
{
    public static int Result;
    public static int Cleanups;

    private static T Identity<T>(T value) => value;

    public static async Task<int> LoadScoreAsync(int score)
    {
        await AvidContinuations.NextTickAsync();
        await AvidContinuations.NextTickAsync();
        int total = 0;
        try
        {
            foreach (int value in new[] { 3, 5, 8 })
            {
                total += Identity<int>(value);
            }
            return score + total;
        }
        finally
        {
            Cleanups++;
        }
    }

    public static async Task<int> RunScenarioAsync()
    {
        int adjustment = 1;
        Task<int> left = LoadScoreAsync(7);
        adjustment = adjustment + 1;
        Task<int> leftAlias = left;
        adjustment = adjustment + 1;
        Task<int> right = LoadScoreAsync(5);
        await AvidContinuations.NextTickAsync();
        int first = await leftAlias;
        int second = await right;
        return first * 10 + second + Cleanups + adjustment - 3;
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static async void BeginPlay()
    {
        int result = 0;
        result = await RunScenarioAsync();
        Result = result;
    }
}
