<#
.SYNOPSIS
    Writes the tag's version into res/app.rc before a release build.

.DESCRIPTION
    res/app.rc carries 1.0.0.0 so a local build.bat always works without
    arguments. A tagged build overwrites it from the tag, so the exe's
    Properties > Details pane matches the release it shipped in.

    Throws if nothing changed, rather than shipping a binary stamped with
    the wrong version because a pattern drifted.
#>
param(
    [Parameter(Mandatory)][string]$Tag,
    [string]$Rc = 'res\app.rc'
)

$ErrorActionPreference = 'Stop'

$version = $Tag -replace '^v', ''
if ($version -notmatch '^\d+(\.\d+)*$') {
    throw "Tag '$Tag' is not a version like v1.2.3"
}

# Pad to the four parts VERSIONINFO wants: v1.2 -> 1,2,0,0
$parts  = @($version -split '\.') + @('0', '0', '0', '0')
$quad   = ($parts[0..3]) -join ','
$dotted = ($parts[0..3]) -join '.'

$before = Get-Content $Rc -Raw
# No end anchor on the first two: the file is CRLF, and a trailing "$" will
# not match because \r is left over after the digit class.
$after  = $before `
    -replace '(?m)^FILEVERSION[ \t]+[\d, \t]+',    "FILEVERSION    $quad" `
    -replace '(?m)^PRODUCTVERSION[ \t]+[\d, \t]+', "PRODUCTVERSION $quad" `
    -replace '(VALUE[ \t]+"FileVersion",[ \t]*)"[^"]*"',    "`$1`"$dotted`"" `
    -replace '(VALUE[ \t]+"ProductVersion",[ \t]*)"[^"]*"', "`$1`"$dotted`""

if ($after -eq $before) {
    throw "Nothing was stamped in $Rc - the VERSIONINFO patterns no longer match"
}

Set-Content -Path $Rc -Value $after -NoNewline
Write-Host "Stamped $Rc with $dotted"
Select-String -Path $Rc -Pattern 'FILEVERSION|PRODUCTVERSION|"FileVersion"|"ProductVersion"' |
    ForEach-Object { Write-Host "  $($_.Line.Trim())" }
