namespace AvidScript.GeneratedBindingSmoke;

public static class GeneratedProjectionSmoke
{
    public static int ReadReservedNames(UAvidScriptCSharpBindingEmitterTestObject target)
        => target.ReservedHandleNames(1, 2);

    public static FTransform PassTransform(FTransform value)
        => UAvidScriptCSharpBindingEmitterTestObject.StaticTransform(value);

    public static void CallRequiredDefaults(
        UAvidScriptCSharpBindingEmitterTestObject target,
        EAvidScriptCSharpEmitterTestMode mode)
    {
        target.OptionalProjection(false, mode);
        target.InvalidScalarDefault(1);
    }

    public static void CallOptionalDefaults(UAvidScriptCSharpBindingEmitterTestObject target)
        => target.OptionalProjection();

    public static bool CheckStaticOwnerDefaultHandle()
        => UAvidScriptCSharpBindingEmitterStaticOwnerTestObject.HasValue(default);
}
