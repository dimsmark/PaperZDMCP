[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$EngineRoot,
    [Parameter(Mandatory)][string]$PaperZDPath
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$engine = (Resolve-Path -LiteralPath $EngineRoot).Path
$paper = (Resolve-Path -LiteralPath $PaperZDPath).Path
if (!(Test-Path -LiteralPath (Join-Path $paper 'PaperZD.uplugin'))) { throw 'PaperZDPath must contain PaperZD.uplugin.' }
$hostRoot = Join-Path $repoRoot ('.build/test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $hostRoot | Out-Null
Copy-Item -Path (Join-Path $repoRoot 'tests/TestHost/*') -Destination $hostRoot -Recurse
$plugins = Join-Path $hostRoot 'Plugins'
New-Item -ItemType Directory -Path $plugins | Out-Null
New-Item -ItemType Directory -Path (Join-Path $plugins 'PaperZDMCP') | Out-Null
Copy-Item -LiteralPath (Join-Path $repoRoot 'PaperZDMCP/Source') -Destination (Join-Path $plugins 'PaperZDMCP') -Recurse
Copy-Item -LiteralPath (Join-Path $repoRoot 'PaperZDMCP/PaperZDMCP.uplugin') -Destination (Join-Path $plugins 'PaperZDMCP')
$paperDestination = Join-Path $plugins 'PaperZD'
New-Item -ItemType Directory -Path $paperDestination | Out-Null
foreach ($entry in @('Source', 'Config', 'Content', 'Resources', 'PaperZD.uplugin')) {
    $inputPath = Join-Path $paper $entry
    if (Test-Path -LiteralPath $inputPath) { Copy-Item -LiteralPath $inputPath -Destination $paperDestination -Recurse }
}
$project = Join-Path $hostRoot 'TestHost.uproject'
& (Join-Path $engine 'Engine/Build/BatchFiles/Build.bat') TestHostEditor Win64 Development "-Project=$project" -WaitMutex -NoHotReloadFromIDE "-Log=$hostRoot/build.log"
if ($LASTEXITCODE -ne 0) { throw "Build failed. See $hostRoot/build.log" }
$previousCompatibility = $env:__COMPAT_LAYER
try {
    # Avoid a false minimum-resolution failure on high-DPI/portrait displays.
    # This applies only to the child process, never to Windows display settings.
    $env:__COMPAT_LAYER = "$previousCompatibility HIGHDPIAWARE".Trim()
    & (Join-Path $engine 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe') $project -unattended -nop4 -nosplash -nullrhi -nosound -PaperZDMCPScratchProject '-ExecCmds=Automation RunTests PaperZDMCP' '-TestExit=Automation Test Queue Empty' "-ReportExportPath=$hostRoot/report" "-abslog=$hostRoot/automation.log"
    if ($LASTEXITCODE -ne 0) { throw "Unreal test process failed. See $hostRoot/automation.log" }
    $report = Get-Content -LiteralPath (Join-Path $hostRoot 'report/index.json') -Raw | ConvertFrom-Json
    if ($report.failed -gt 0 -or ($report.succeeded + $report.succeededWithWarnings) -lt 4) { throw "Automation tests did not all pass. See $hostRoot/report/index.json" }
    # The first editor has exited. A second OS process must load the saved
    # owner fixture from disk; reloading a package alone is not this test.
    & (Join-Path $engine 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe') $project -unattended -nop4 -nosplash -nullrhi -nosound -PaperZDMCPScratchProject -PaperZDMCPColdStart '-ExecCmds=Automation RunTests PaperZDMCP.OwnerPersistence' '-TestExit=Automation Test Queue Empty' "-ReportExportPath=$hostRoot/cold-start-report" "-abslog=$hostRoot/cold-start.log"
    if ($LASTEXITCODE -ne 0) { throw "Cold-start test process failed. See $hostRoot/cold-start.log" }
    $coldReport = Get-Content -LiteralPath (Join-Path $hostRoot 'cold-start-report/index.json') -Raw | ConvertFrom-Json
    if ($coldReport.failed -gt 0 -or ($coldReport.succeeded + $coldReport.succeededWithWarnings) -ne 1) { throw "Cold-start persistence test failed. See $hostRoot/cold-start-report/index.json" }
} finally {
    $env:__COMPAT_LAYER = $previousCompatibility
}
Write-Output "Tests passed. Evidence retained at $hostRoot"
