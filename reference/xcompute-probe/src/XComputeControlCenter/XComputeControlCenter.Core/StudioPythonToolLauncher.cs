using System.Diagnostics;

namespace XComputeControlCenter.Core;

internal static class StudioPythonToolLauncher
{
    private const string IsolatedToolBootstrap =
        "import os,runpy,sys;" +
        "script=os.path.abspath(sys.argv[1]);" +
        "sys.argv=sys.argv[1:];" +
        "sys.path.insert(0,os.path.dirname(script));" +
        "runpy.run_path(script,run_name='__main__')";

    public static void Configure(
        ProcessStartInfo startInfo,
        string toolPath,
        IEnumerable<string> arguments)
    {
        startInfo.Environment.Remove("PYTHONHOME");
        startInfo.Environment.Remove("PYTHONPATH");
        startInfo.Environment.Remove("PYTHONSTARTUP");
        startInfo.Environment["PYTHONNOUSERSITE"] = "1";
        startInfo.Environment["PYTHONDONTWRITEBYTECODE"] = "1";
        startInfo.Environment["PYTHONUTF8"] = "1";

        // The CPython embedded distribution deliberately omits the script
        // directory from sys.path. Add only the manifest-bound tool directory
        // while retaining isolated mode and ignoring ambient Python state.
        startInfo.ArgumentList.Add("-B");
        startInfo.ArgumentList.Add("-I");
        startInfo.ArgumentList.Add("-c");
        startInfo.ArgumentList.Add(IsolatedToolBootstrap);
        startInfo.ArgumentList.Add(toolPath);
        foreach (var argument in arguments)
        {
            startInfo.ArgumentList.Add(argument);
        }
    }
}
