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

    [UProperty(BlueprintReadOnly = true, Category = "Projectile")]
    public int DamageRepNotifyCount { get; private set; }

    [UProperty(BlueprintReadOnly = true, Category = "Projectile")]
    public int TickCount { get; private set; }

    [UProperty(BlueprintReadOnly = true, Category = "Projectile")]
    public int EndPlayCount { get; private set; }

    [UFunction(BlueprintCallable = true, BlueprintNativeEvent = true, Category = "Projectile")]
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
        DamageRepNotifyCount += 1;
    }

    protected override void BeginPlay()
    {
        HasBegunPlay = true;
    }

    protected override void Tick(float deltaSeconds)
    {
        TickCount += 1;
    }

    protected override void EndPlay()
    {
        EndPlayCount += 1;
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

    [UProperty(BlueprintReadOnly = true, Category = "Health")]
    public int BeginPlayCount { get; private set; }

    [UProperty(BlueprintReadOnly = true, Category = "Health")]
    public int TickCount { get; private set; }

    [UProperty(BlueprintReadOnly = true, Category = "Health")]
    public int EndPlayCount { get; private set; }

    protected override void BeginPlay()
    {
        BeginPlayCount += 1;
    }

    protected override void Tick(float deltaSeconds)
    {
        TickCount += 1;
    }

    protected override void EndPlay()
    {
        EndPlayCount += 1;
    }
}

[UClass]
public partial class EncounterSubsystem : AvidWorldSubsystem
{
    [UProperty(BlueprintReadOnly = true, Category = "Encounter")]
    public int InitializeCount { get; private set; }

    [UProperty(BlueprintReadOnly = true, Category = "Encounter")]
    public int TickCount { get; private set; }

    [UProperty(BlueprintReadOnly = true, Category = "Encounter")]
    public int DeinitializeCount { get; private set; }

    [UFunction(BlueprintCallable = true, Category = "Encounter")]
    public int GetActiveEncounterCount()
    {
        return InitializeCount;
    }

    protected override void Initialize()
    {
        InitializeCount += 1;
    }

    protected override void Tick(float deltaSeconds)
    {
        TickCount += 1;
    }

    protected override void Deinitialize()
    {
        DeinitializeCount += 1;
    }
}

[UClass]
public partial class ProfileSubsystem : AvidGameInstanceSubsystem
{
    [UProperty(BlueprintReadOnly = true, Category = "Profile")]
    public int InitializeCount { get; private set; }

    [UProperty(BlueprintReadOnly = true, Category = "Profile")]
    public int DeinitializeCount { get; private set; }

    protected override void Initialize()
    {
        InitializeCount += 1;
    }

    protected override void Deinitialize()
    {
        DeinitializeCount += 1;
    }
}
