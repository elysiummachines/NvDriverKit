<#
.SYNOPSIS
    Writes the tag's version into the files that carry it.

.DESCRIPTION
    Two places hold a version a user can see:
      res\app.rc    VERSIONINFO, shown in Properties > Details
      src\common.h  NVDK_VERSION, shown in the window title

    Both carry 1.0.0 in the repository so a local build.bat works without
    arguments, and a tagged build overwrites them from the tag.

    Each pattern must match exactly once, which catches a drifted or
    duplicated line rather than shipping a binary stamped with the wrong
    version. It deliberately does not care whether the text changed:
    stamping v1.0.0 onto a file that already reads 1.0.0 is a legitimate
    no-op.
#>
param(
    [Parameter(Mandatory)][string]$Tag,
    [string]$Rc     = 'res\app.rc',
    [string]$Header = 'src\common.h'
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
$short  = ($parts[0..2]) -join '.'   # the title bar shows three parts

# No end anchor on the first two: the file is CRLF, and a trailing "$" will
# not match because \r is left over after the digit class.
$targets = @(
    @{ Path = $Rc; Edits = @(
        @{ Name = 'FILEVERSION'
           Pattern = '(?m)^FILEVERSION[ \t]+[\d, \t]+'
           Replace = "FILEVERSION    $quad" }
        @{ Name = 'PRODUCTVERSION'
           Pattern = '(?m)^PRODUCTVERSION[ \t]+[\d, \t]+'
           Replace = "PRODUCTVERSION $quad" }
        @{ Name = 'VALUE "FileVersion"'
           Pattern = '(VALUE[ \t]+"FileVersion",[ \t]*)"[^"]*"'
           Replace = '$1"' + $dotted + '"' }
        @{ Name = 'VALUE "ProductVersion"'
           Pattern = '(VALUE[ \t]+"ProductVersion",[ \t]*)"[^"]*"'
           Replace = '$1"' + $dotted + '"' }
    )}
    @{ Path = $Header; Edits = @(
        @{ Name = 'NVDK_VERSION'
           Pattern = '(#define[ \t]+NVDK_VERSION[ \t]+L")[^"]*(")'
           Replace = '${1}' + $short + '${2}' }
    )}
)

foreach ($target in $targets) {
    $text = Get-Content $target.Path -Raw
    foreach ($edit in $target.Edits) {
        $hits = [regex]::Matches($text, $edit.Pattern).Count
        if ($hits -ne 1) {
            throw "Expected exactly one $($edit.Name) line in $($target.Path), found $hits - the version patterns have drifted"
        }
        $text = [regex]::Replace($text, $edit.Pattern, $edit.Replace)
    }
    Set-Content -Path $target.Path -Value $text -NoNewline
    Write-Host "Stamped $($target.Path)"
}

Write-Host ""
Write-Host "Version $dotted (title bar $short):"
Select-String -Path $Rc -Pattern 'FILEVERSION|PRODUCTVERSION|"FileVersion"|"ProductVersion"' |
    ForEach-Object { Write-Host "  $($_.Line.Trim())" }
Select-String -Path $Header -Pattern 'NVDK_VERSION' |
    ForEach-Object { Write-Host "  $($_.Line.Trim())" }
