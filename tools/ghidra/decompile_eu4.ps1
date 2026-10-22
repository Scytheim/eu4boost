# eu4.exe 전체 디컴파일 스크립트 (PC에서 실행)
# 같은 폴더의 decompile_eu4.cmd 를 더블클릭해서 실행하세요. 관리자 권한은 필요 없습니다.
#
# - 이미 분석해 둔 Ghidra 프로젝트(C:\tools\eu4_ghidra\eu4.gpr)를 읽기 전용으로 엽니다. 재분석하지 않습니다.
# - 모든 함수의 의사 C 코드를 이 폴더의 ghidra_decomp 에 저장합니다 (수백 MB가 될 수 있음).
# - 디컴파일 중에는 CPU를 많이 씁니다. 창을 닫으면 중단됩니다.

$ErrorActionPreference = 'Continue'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}

$GhidraDir  = 'C:\tools\ghidra_12.1.4_PUBLIC'
$JdkDir     = 'C:\tools\jdk-21'
$ProjectDir = 'C:\tools\eu4_ghidra'
$Here       = Split-Path -Parent $MyInvocation.MyCommand.Path
$OutDir     = Join-Path $Here 'ghidra_decomp'
$LogDir     = Join-Path $Here 'ghidra_logs'
$StatusFile = Join-Path $Here 'ghidra_decomp_status.txt'
$Headless   = Join-Path $GhidraDir 'support\analyzeHeadless.bat'

function Save-Status([string]$state, [string]$extra) {
  $lines = @("time: $(Get-Date -Format s)", "status: $state", $extra)
  [System.IO.File]::WriteAllLines($StatusFile, [string[]]$lines, (New-Object System.Text.UTF8Encoding($false)))
}

Write-Host '=== eu4.exe 전체 디컴파일 ===' -ForegroundColor Green
foreach ($p in @($Headless, (Join-Path $JdkDir 'bin\java.exe'), (Join-Path $ProjectDir 'eu4.gpr'), (Join-Path $Here 'DecompileAll.java'))) {
  if (-not (Test-Path -LiteralPath $p)) { Write-Host "  [중단] 필요한 파일이 없습니다: $p" -ForegroundColor Red; Save-Status 'failed' "missing: $p"; Read-Host '엔터를 누르면 창이 닫힙니다'; exit 1 }
}
foreach ($d in @($OutDir, $LogDir)) { New-Item -ItemType Directory -Path $d -Force | Out-Null }

$ramGb = [math]::Floor((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)
$heapGb = [math]::Max(4, [math]::Min(16, [math]::Floor($ramGb / 2)))
$env:JAVA_HOME = $JdkDir
$env:GHIDRA_HEADLESS_MAXMEM = "${heapGb}G"

Write-Host "  Ghidra 메모리 ${heapGb}GB, 결과 폴더: $OutDir"
Write-Host '  함수 6만여 개를 디컴파일합니다. 보통 10~40분 걸립니다. 진행 기록이 계속 올라옵니다.' -ForegroundColor Yellow
Save-Status 'running' ''
$started = Get-Date
& $Headless $ProjectDir 'eu4' -process 'eu4.exe' -noanalysis -readOnly -scriptPath $Here -postScript 'DecompileAll.java' $OutDir 2000 60 -log (Join-Path $LogDir 'decomp.log') -scriptlog (Join-Path $LogDir 'decomp_script.log')
$code = $LASTEXITCODE
$minutes = [math]::Round(((Get-Date) - $started).TotalMinutes, 1)

$summary = Join-Path $OutDir 'summary.txt'
if (Test-Path -LiteralPath $summary) {
  $sizeMb = [math]::Round(((Get-ChildItem -LiteralPath $OutDir -File | Measure-Object Length -Sum).Sum) / 1MB, 1)
  Save-Status 'finished' ("minutes: $minutes, exit: $code, output_mb: $sizeMb`n" + ((Get-Content -LiteralPath $summary) -join "`n"))
  Write-Host ''
  Write-Host "끝났습니다 (${minutes}분, ${sizeMb}MB). Claude 채팅에 '디컴파일 끝났어'라고 알려 주세요." -ForegroundColor Green
} else {
  Save-Status 'failed' "minutes: $minutes, exit: $code (ghidra_logs\decomp_script.log 확인)"
  Write-Host '  [실패] summary.txt 가 없습니다. ghidra_logs\decomp_script.log 를 확인해 주세요.' -ForegroundColor Red
}
Read-Host '엔터를 누르면 창이 닫힙니다'
