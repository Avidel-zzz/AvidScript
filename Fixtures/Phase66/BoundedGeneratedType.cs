using System;
using AvidScript;

namespace AvidScriptSamples;

[UClass]
public partial class BoundedErrorActor : AvidActor
{
    [UFunction(BlueprintCallable = true)]
    public int ReadOrFallback()
    {
        try
        {
            return Fail();
        }
        catch (Exception)
        {
            return 7;
        }
    }

    [UFunction(BlueprintCallable = true)]
    public void RaiseUncaught()
    {
        Fail();
    }

    private static int Fail()
    {
        throw new Exception();
    }
}
