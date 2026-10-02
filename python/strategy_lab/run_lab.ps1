<#
.SYNOPSIS
  Starts the Strategy Lab UI on http://127.0.0.1:<port> (this computer only).

.DESCRIPTION
  Checks the prerequisites and says what to do if one is missing. Does not build the C++ replay tool and does not
  install anything. Press Ctrl+C in this window to stop the lab.

  Run from the repository root:
    powershell -NoProfile -ExecutionPolicy Bypass -File python\strategy_lab\run_lab.ps1
  Options:  -Port 8501   -Exe <path to strategy_lab_replay.exe>   -NoBrowser (do not open a browser tab)
#>
[CmdletBinding()]
param(
    [int]$Port = 8501,
    [string]$Exe = "",
    [switch]$NoBrowser
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here "..\..")).Path
$venvPython = Join-Path $here ".venv\Scripts\python.exe"

function Fail([string]$message) {
    Write-Host ""
    Write-Host "CANNOT START: $message" -ForegroundColor Red
    exit 1
}

# 1. The isolated Python environment.
if (-not (Test-Path $venvPython)) {
    Fail "the Python environment does not exist yet. Run once:  powershell -NoProfile -ExecutionPolicy Bypass -File python\strategy_lab\setup.ps1"
}
$saved = $ErrorActionPreference
$ErrorActionPreference = "Continue"
& $venvPython -c "import streamlit, plotly" 2>$null
$importCode = $LASTEXITCODE
$ErrorActionPreference = $saved
if ($importCode -ne 0) {
    Fail "streamlit or plotly cannot be imported from the environment. Repair it with:  powershell -NoProfile -ExecutionPolicy Bypass -File python\strategy_lab\setup.ps1 -Recreate"
}

# 2. The C++ replay tool (built once, separately).
$candidates = @()
if ($Exe) { $candidates += $Exe }
elseif ($env:STRATEGY_LAB_EXE) { $candidates += $env:STRATEGY_LAB_EXE }
else {
    foreach ($config in @("Release", "RelWithDebInfo", "MinSizeRel", "Debug")) {
        $candidates += (Join-Path $repo "out\strategy_lab\bin\$config\strategy_lab_replay.exe")
    }
}
$found = $candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if (-not $found) {
    Write-Host ""
    Write-Host "CANNOT START: strategy_lab_replay.exe has not been built." -ForegroundColor Red
    Write-Host "Looked for:"
    $candidates | ForEach-Object { Write-Host "  $_" }
    Write-Host ""
    Write-Host "Build it once, from the repository root (Visual Studio 2022 with the C++ workload and CMake are needed):"
    Write-Host '  cmake -S . -B out/strategy_lab -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=OFF -DTRADING_ENGINE_BUILD_STRATEGY_LAB=ON'
    Write-Host '  cmake --build out/strategy_lab --config Release --target strategy_lab_replay'
    Write-Host "then run this script again (or pass -Exe <path>)."
    exit 1
}
$env:STRATEGY_LAB_EXE = (Resolve-Path -LiteralPath $found).Path

# 3. The app-scoped Streamlit configuration (dark theme, 127.0.0.1 only). Streamlit reads .streamlit\config.toml from the directory
#    it is started in, and step 4 starts it from $here, so this is the file that applies.
$config = Join-Path $here ".streamlit\config.toml"
if (-not (Test-Path -LiteralPath $config -PathType Leaf)) {
    Fail "the Strategy Lab's Streamlit configuration is missing: $config (it sets the dark theme and the local-only address)."
}
$themeOverrides = @(Get-ChildItem Env: | Where-Object { $_.Name -like "STREAMLIT_THEME*" } | ForEach-Object { $_.Name })
if ($themeOverrides.Count -gt 0) {
    Write-Host ("NOTE: " + ($themeOverrides -join ", ") + " is set in this environment and overrides the lab's theme settings.") -ForegroundColor Yellow
}

# 4. The port.
$listener = $null
try {
    $listener = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Parse("127.0.0.1"), $Port)
    $listener.Start()
} catch {
    Fail "port $Port on 127.0.0.1 is already in use. Close the other program (maybe a lab already running), or pick another with -Port 8502."
} finally {
    if ($listener) { $listener.Stop() }
}

$url = "http://127.0.0.1:$Port"
Write-Host "Strategy Lab: $url   (local only; Ctrl+C to stop)"
Write-Host "Replay tool:  $env:STRATEGY_LAB_EXE"
Write-Host "Config:       $config"
if (-not $NoBrowser) {
    # Open the browser a few seconds from now, once the server is listening.
    Start-Process -WindowStyle Hidden -FilePath "cmd.exe" -ArgumentList "/c", "timeout /t 4 /nobreak >nul & start `"`" $url" | Out-Null
}

# 5. Start. Headless so Streamlit never asks for an e-mail address or opens a tab itself; bound to localhost only.
Set-Location $here
$ErrorActionPreference = "Continue"
& $venvPython -m streamlit run app.py --server.address 127.0.0.1 --server.port $Port --server.headless true --browser.gatherUsageStats false
exit $LASTEXITCODE
