using AvidScript;

namespace AvidScriptSamples;

[UClass(Blueprintable = true, BlueprintType = true)]
public partial class Projectile : AvidActor
{
    [UProperty(
        EditAnywhere = true,
        BlueprintReadWrite = true,
        ReplicatedUsing = nameof(OnRepDamage),
        Category = "Projectile")]
    public float Damage { get; set; } = 25.0f;

    [UFunction(BlueprintCallable = true, Category = "Projectile")]
    public virtual void Activate(float damageScale)
    {
        Damage *= damageScale;
    }

    [UFunction]
    private void OnRepDamage()
    {
    }

    protected override void BeginPlay()
    {
    }
}

[UClass(Blueprintable = true)]
public partial class ExplosiveProjectile : Projectile
{
    [UFunction(BlueprintCallable = true, Category = "Projectile")]
    public override void Activate(float damageScale)
    {
        base.Activate(damageScale * 2.0f);
    }
}

[UClass(BlueprintType = true)]
public partial class HealthComponent : AvidActorComponent
{
    [UProperty(BlueprintReadOnly = true, Category = "Health")]
    public float CurrentHealth { get; private set; } = 100.0f;
}

[UClass]
public partial class EncounterSubsystem : AvidWorldSubsystem
{
    [UFunction(BlueprintCallable = true, Category = "Encounter")]
    public int GetActiveEncounterCount()
    {
        return 0;
    }
}
