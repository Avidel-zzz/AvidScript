#requires -Version 7.0
Set-StrictMode -Version Latest

function Assert-AvidScriptUiSaveSafePath {
    param([string]$Path)
    if (-not [IO.Path]::IsPathFullyQualified($Path) -or
        [IO.Path]::GetPathRoot($Path) -notmatch '\A[A-Za-z]:[\\/]\z' -or
        -not (Test-AvidScriptBindingPathContained -RootPath ([IO.Path]::GetPathRoot($Path)) -CandidatePath $Path)) {
        throw "Path must be absolute, local and without reparse points: $Path"
    }
    $Current = [IO.Path]::GetFullPath($Path)
    while ($Current) {
        try {
            $Entry = Get-Item -LiteralPath $Current -Force -ErrorAction Stop
            if (($Entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw "Reparse point is forbidden: $Current" }
        } catch [Management.Automation.ItemNotFoundException] { }
        $Parent = Split-Path -Parent $Current
        if ($Parent -eq $Current) { break }
        $Current = $Parent
    }
}

function Assert-AvidScriptUiSaveUserRoot {
    param([string]$Path, [object]$Context)
    Assert-AvidScriptUiSaveSafePath $Path
    $Full = [IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
    foreach ($Protected in @((Split-Path -Parent $Context.project), $Context.engine)) {
        $Protected = [IO.Path]::GetFullPath($Protected).TrimEnd('\', '/')
        if ($Full.Equals($Protected, [StringComparison]::OrdinalIgnoreCase) -or
            $Full.StartsWith($Protected + '\', [StringComparison]::OrdinalIgnoreCase) -or
            $Protected.StartsWith($Full + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw 'VerifyUserRoot must be outside, and not an ancestor of, project and engine directories.'
        }
    }
}

function New-AvidScriptUiSaveDirectory {
    param([string]$Path)
    Assert-AvidScriptUiSaveSafePath $Path
    [void][IO.Directory]::CreateDirectory((Split-Path -Parent $Path))
    Assert-AvidScriptUiSaveSafePath $Path
    if (-not ('AvidScriptUiSaveNativeDirectories' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class AvidScriptUiSaveNativeDirectories {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CreateDirectoryW(string path, IntPtr security);
    public static void CreateNew(string path) {
        if (!CreateDirectoryW(path, IntPtr.Zero))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Evidence directory must be new: " + path);
    }
}
'@
    }
    [AvidScriptUiSaveNativeDirectories]::CreateNew($Path)
    Assert-AvidScriptUiSaveSafePath $Path
}

function Write-AvidScriptUiSaveNewJson {
    param([string]$Path, [object]$Value)
    Assert-AvidScriptUiSaveSafePath $Path
    $Bytes = [Text.UTF8Encoding]::new($false).GetBytes(($Value | ConvertTo-Json -Depth 32 -Compress))
    $File = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $File.Write($Bytes, 0, $Bytes.Length) } finally { $File.Dispose() }
}

function Test-AvidScriptUiSaveSamePath {
    param([object]$Actual, [string]$Expected)
    return $Actual -is [string] -and [IO.Path]::IsPathFullyQualified($Actual) -and
        [IO.Path]::GetFullPath($Actual).TrimEnd('\', '/').Equals(
            [IO.Path]::GetFullPath($Expected).TrimEnd('\', '/'), [StringComparison]::OrdinalIgnoreCase)
}

function Assert-AvidScriptUiSaveSaveDirectory {
    param([string]$SavePath)
    Assert-AvidScriptUiSaveSafePath $SavePath
    $Directory = Split-Path -Parent $SavePath
    if (Test-Path -LiteralPath $Directory) {
        foreach ($Entry in Get-ChildItem -LiteralPath $Directory -Force) {
            if ($Entry.PSIsContainer -or -not (Test-AvidScriptUiSaveSamePath $Entry.FullName $SavePath)) {
                throw 'Isolated SaveGames directory contains an unexpected entry.'
            }
            Assert-AvidScriptUiSaveSafePath $Entry.FullName
        }
    }
}
