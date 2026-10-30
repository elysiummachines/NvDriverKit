<#
.SYNOPSIS
    Checks the built exe against the claims the README makes about it.

.DESCRIPTION
    Run after build.bat. Fails the job if any of these regress:
      - the binary exists and is a plausible size
      - ASLR, high-entropy ASLR and DEP are all set
      - exactly one embedded manifest, requesting requireAdministrator,
        with no asInvoker (a second manifest means the elevation request
        silently stopped working)
      - no MinGW runtime DLLs leaked into the imports
#>
param([string]$Exe = 'build\NvDriverKit.exe')

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Exe)) { throw "Not built: $Exe" }
$item = Get-Item $Exe
Write-Host "Binary : $($item.FullName)"
Write-Host "Size   : $($item.Length) bytes"
if ($item.Length -lt 40KB) { throw "Suspiciously small: $($item.Length) bytes" }

# ---- DllCharacteristics, read straight out of the PE optional header ----
# e_lfanew lives at 0x3C; the optional header starts 0x18 past the PE
# signature, and DllCharacteristics sits at 0x46 within it (PE32+).
$bytes = [IO.File]::ReadAllBytes($item.FullName)
$peOff = [BitConverter]::ToInt32($bytes, 0x3C)
if ([BitConverter]::ToUInt32($bytes, $peOff) -ne 0x00004550) { throw 'Not a PE file' }
$dllChar = [BitConverter]::ToUInt16($bytes, $peOff + 0x5E)
Write-Host ("DllChar: 0x{0:X4}" -f $dllChar)

$required = [ordered]@{
    'High-entropy ASLR'   = 0x0020
    'ASLR (dynamic base)' = 0x0040
    'DEP (NX compat)'     = 0x0100
}
$missing = @()
foreach ($flag in $required.GetEnumerator()) {
    $on = ($dllChar -band $flag.Value) -ne 0
    Write-Host ("  {0,-20} {1}" -f $flag.Key, $(if ($on) { 'on' } else { 'OFF' }))
    if (-not $on) { $missing += $flag.Key }
}
if ($missing.Count) { throw "Hardening flags missing: $($missing -join ', ')" }

# ---- manifest ----
# The resource is stored as UTF-8, so counting occurrences in the raw bytes
# is enough to catch the dual-manifest failure without parsing resources.
$text    = [Text.Encoding]::UTF8.GetString($bytes)
$admin   = [regex]::Matches($text, 'requireAdministrator').Count
$invoker = [regex]::Matches($text, 'asInvoker').Count
Write-Host "Manifest: requireAdministrator x$admin, asInvoker x$invoker"
if ($admin -ne 1)   { throw "Expected exactly one requireAdministrator, found $admin" }
if ($invoker -ne 0) { throw "Found $invoker asInvoker - MinGW's default manifest was not shadowed" }

# ---- imports ----
$dlls = objdump -p $Exe |
    Select-String 'DLL Name:' |
    ForEach-Object { ($_ -replace '.*DLL Name:\s*', '').Trim() }
Write-Host "Imports ($($dlls.Count)):"
$dlls | Sort-Object | ForEach-Object { Write-Host "  $_" }

$leaked = $dlls | Where-Object { $_ -match '^(libgcc|libssp|libwinpthread|libstdc)' }
if ($leaked) { throw "Non-system DLL dependency leaked in: $($leaked -join ', ')" }

Write-Host ''
Write-Host 'All binary checks passed.'
