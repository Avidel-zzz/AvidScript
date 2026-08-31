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
    public float LastReplicatedDamage { get; private set; }

    [UProperty(BlueprintReadOnly = true, Category = "Projectile")]
    public int ServerRpcCount { get; private set; }

    [UProperty(BlueprintReadOnly = true, Category = "Projectile")]
    public float LastServerDamage { get; private set; }

    [UProperty(BlueprintReadOnly = true, Category = "Projectile")]
    public int ClientAckCount { get; private set; }

    [UProperty(BlueprintReadOnly = true, Category = "Projectile")]
    public float LastClientAckDamage { get; private set; }

    [UProperty(BlueprintReadOnly = true, Category = "Projectile")]
    public int MulticastCount { get; private set; }

    [UProperty(BlueprintReadOnly = true, Category = "Projectile")]
    public float LastMulticastDamage { get; private set; }

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

    [UFunction(BlueprintCallable = true, Category = "Projectile")]
    public void ConfigureLaunch(float speed = 1200.0f, bool homing = false, int burstCount = 1)
    {
        PrecisionScale = speed;
        IsActive = homing;
        ActivationCount += burstCount;
    }

    [UFunction(Server = true, Reliable = true)]
    public void ServerSubmitDamage(float submittedDamage)
    {
        Damage = submittedDamage;
        ServerRpcCount += 1;
        LastServerDamage = submittedDamage;
    }

    [UFunction(Client = true, Reliable = true)]
    public void ClientConfirmDamage(float confirmedDamage)
    {
        ClientAckCount += 1;
        LastClientAckDamage = confirmedDamage;
    }

    [UFunction(NetMulticast = true, Reliable = true)]
    public void MulticastObserveDamage(float observedDamage)
    {
        MulticastCount += 1;
        LastMulticastDamage = observedDamage;
    }

    [UFunction]
    private void OnRepDamage()
    {
        DamageRepNotifyCount += 1;
        LastReplicatedDamage = Damage;
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
