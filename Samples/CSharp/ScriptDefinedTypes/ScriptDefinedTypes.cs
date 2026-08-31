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

    [UProperty(BlueprintReadWrite = true, Category = "Projectile")]
    public int ActivationCount { get; set; } = 1;

    [UProperty(BlueprintReadWrite = true, Category = "Projectile")]
    public long AccumulatedDamage { get; set; } = 100L;

    [UProperty(BlueprintReadWrite = true, Category = "Projectile")]
    public double PrecisionScale { get; set; } = 1.5;

    [UProperty(BlueprintReadWrite = true, Category = "Projectile")]
    public bool IsActive { get; set; }

    [UProperty(BlueprintReadOnly = true, Category = "Projectile")]
    public bool HasBegunPlay { get; private set; }

    [UFunction(BlueprintCallable = true, Category = "Projectile")]
    public virtual void Activate(float damageScale)
    {
        Damage *= damageScale;
        ActivationCount += 1;
        AccumulatedDamage += 25L;
        PrecisionScale *= 2.0;
        IsActive = true;
    }

    [UFunction(BlueprintPure = true, Category = "Projectile")]
    public int GetActivationCount()
    {
        return ActivationCount;
    }

    [UFunction]
    private void OnRepDamage()
    {
    }

    protected override void BeginPlay()
    {
        HasBegunPlay = true;
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
