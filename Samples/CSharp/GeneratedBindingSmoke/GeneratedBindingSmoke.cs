using AvidScript;

namespace AvidScriptSamples;

public static class GeneratedBindingSmoke
{
    public static FVector ReadActorLocation()
    {
        return UE.Self.GetActorLocation();
    }

    public static USceneComponent ReadRootComponent()
    {
        return UE.Self.GetRootComponent();
    }

    public static void ApplyUniformScale(float scale)
    {
        UE.Self.SetActorScale3D(new FVector(scale, scale, scale));
    }
}
