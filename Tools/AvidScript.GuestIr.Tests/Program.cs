using System;

internal static class Program
{
    private static int Main()
    {
        try
        {
            int count = GuestModuleValidationTests.Run() + GuestIrSerializationTests.Run()
                + GuestDataLayoutTests.Run() + GuestValidatorRobustnessTests.Run()
                + GuestArrayInstructionTests.Run() + GuestLanguageOutcomeTypeTests.Run()
                + GuestLanguageOutcomeFlowTests.Run() + GuestLanguageErrorCatalogTests.Run()
                + GuestTaskCancellationErrorTests.Run() + GuestStaticStorageTests.Run();
            Console.WriteLine($"AvidScript.GuestIr.Tests: {count}/{count} passed");
            return 0;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(exception);
            return 1;
        }
    }
}
