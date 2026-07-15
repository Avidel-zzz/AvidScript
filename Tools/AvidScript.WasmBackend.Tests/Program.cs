using System;

internal static class Program
{
    private static int Main()
    {
        try
        {
            int count = WasmLeb128Tests.Run() + WasmModuleCompilerTests.Run()
                + WasmArrayBoundsTests.Run() + WasmArtifactInspectionReportTests.Run();
            Console.WriteLine($"AvidScript.WasmBackend.Tests: {count}/{count} passed");
            return 0;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(exception);
            return 1;
        }
    }
}
