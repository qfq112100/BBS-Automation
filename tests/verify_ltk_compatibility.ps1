[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Probe,
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string]$DataRoot,
    [Parameter(Mandatory=$true)][string]$RollbackRoot,
    [Parameter(Mandatory=$true)][string]$OutputDir
)
$ErrorActionPreference='Stop'
$full=[IO.Path]::GetFullPath($OutputDir)
$temp=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')+'\'
if(-not $full.StartsWith($temp,[StringComparison]::OrdinalIgnoreCase) -or (Test-Path -LiteralPath $full)){throw 'Requires a fresh TEMP-only output directory'}
New-Item -ItemType Directory -Path $full|Out-Null
$fixture=Join-Path $full 'fixture'
$roseRel='components\rose\1.4.9'
$ltkRel='components\ltk\1.30.4'
$names=@('ltk_patcher_host.exe','ltk_patcher_dll.dll')
$sourceRose=Join-Path $DataRoot $roseRel
$sourceLtk=Join-Path $DataRoot $ltkRel
$fixtureRose=Join-Path $fixture $roseRel
$fixtureLtk=Join-Path $fixture $ltkRel
$targets=@()
foreach($dir in @((Join-Path $sourceRose '_internal\injection\tools'),$sourceLtk)){foreach($name in $names){$targets+=Join-Path $dir $name}}
$targets+=Join-Path $sourceRose '.bbs-component.json'
$targets+=Join-Path $sourceLtk '.bbs-component.json'
foreach($name in @('config.json','tool-center.json')){if(Test-Path -LiteralPath (Join-Path $DataRoot $name)){$targets+=Join-Path $DataRoot $name}}
$before=@{};foreach($p in $targets){$before[$p]=(Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash}
$checks=New-Object 'System.Collections.Generic.List[object]'
function Run-Case([string]$Name,[string]$Root,[bool]$ExpectedPass){
    $log=@(& $Probe $Executable $Root 2>&1)
    $code=$LASTEXITCODE
    $log|Set-Content -LiteralPath (Join-Path $full ($Name+'.log')) -Encoding UTF8
    $pass=($ExpectedPass -and $code -eq 0) -or (-not $ExpectedPass -and $code -ne 0)
    $checks.Add([pscustomobject]@{name=$Name;pass=$pass;exitCode=$code;expectedReady=$ExpectedPass})
    if(-not $pass){throw ('Unexpected runtime result: '+$Name+' exit='+$code+' '+($log -join ';'))}
}
function Assert-FixturePath([string]$Path){
    if(-not [IO.Path]::GetFullPath($Path).StartsWith([IO.Path]::GetFullPath($fixture)+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Mutation escaped isolated fixture'}
}
function Put-Pair([string]$Source,[string]$Destination){
    foreach($name in $names){$target=Join-Path $Destination $name;Assert-FixturePath $target;Copy-Item -LiteralPath (Join-Path $Source $name) -Destination $target -Force}
}
try{
    Run-Case 'actual-fresh-manager-1' $DataRoot $true
    Run-Case 'actual-fresh-manager-2' $DataRoot $true
    New-Item -ItemType Directory -Path (Split-Path -Parent $fixtureRose),(Split-Path -Parent $fixtureLtk) -Force|Out-Null
    Copy-Item -LiteralPath $sourceRose -Destination $fixtureRose -Recurse
    Copy-Item -LiteralPath $sourceLtk -Destination $fixtureLtk -Recurse
    $toolDir=Join-Path $fixtureRose '_internal\injection\tools'
    Run-Case 'official-four-files' $fixture $true
    $dll=Join-Path $toolDir 'ltk_patcher_dll.dll';Assert-FixturePath $dll
    $bytes=[IO.File]::ReadAllBytes($dll);$bytes[$bytes.Length-1]=$bytes[$bytes.Length-1] -bxor 1;[IO.File]::WriteAllBytes($dll,$bytes)
    $tamperHash=(Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash
    Run-Case 'same-size-one-byte-tamper' $fixture $false
    if((Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash -ne $tamperHash){throw 'Rejected file was silently replaced'}
    Put-Pair $sourceLtk $toolDir
    Copy-Item -LiteralPath (Join-Path $RollbackRoot 'component-0\ltk_patcher_dll.dll') -Destination $dll -Force
    Run-Case 'mixed-host-dll-pair' $fixture $false
    Put-Pair (Join-Path $RollbackRoot 'component-0') $toolDir
    Run-Case 'mixed-rose-ltk-folders' $fixture $false
    Put-Pair (Join-Path $RollbackRoot 'component-1') $fixtureLtk
    Run-Case 'original-four-files' $fixture $true
    Put-Pair $sourceLtk $fixtureLtk
    Put-Pair $sourceLtk $toolDir
    $sentinel=Join-Path $fixtureRose 'user-sentinel.txt';Assert-FixturePath $sentinel
    [IO.File]::WriteAllText($sentinel,'preserve user content')
    $parked=Join-Path $fixture 'parked-rose'
    Assert-FixturePath $fixtureRose;Assert-FixturePath $parked
    Move-Item -LiteralPath $fixtureRose -Destination $parked
    Run-Case 'fresh-rose-binds-local-official-ltk' $fixture $true
    if(-not(Test-Path -LiteralPath (Join-Path $parked 'user-sentinel.txt'))){throw 'Parked user content lost'}
    foreach($name in $names){if((Get-FileHash -LiteralPath (Join-Path $toolDir $name) -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath (Join-Path $sourceLtk $name) -Algorithm SHA256).Hash){throw 'Fresh Rose did not bind exact official local bytes'}}
}finally{
    foreach($p in $before.Keys){if((Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash -ne $before[$p]){throw ('Actual user data changed: '+$p)}}
    [ordered]@{checks=$checks.ToArray();actualRuntimeAndMarkersUnchanged=$true;serviceStarted=$false;toolLaunches=0;processStops=0;fixtureRoot=$fixture}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $full 'report.json') -Encoding UTF8
}
'compatibility_cases='+$checks.Count+' failures='+@($checks|Where-Object{-not $_.pass}).Count
