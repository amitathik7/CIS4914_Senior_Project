<#
.SYNOPSIS
  Creates the isolated Python environment (.venv) for the Strategy Lab UI.

.DESCRIPTION
  Needs Python 3.10 or newer and an internet connection ONCE (to download the pinned packages in
  requirements.txt). Installs nothing globally: everything goes into python\strategy_lab\.venv.
  After this, the built-in demos run fully offline. Does not build the C++ replay tool.

  Run from the repository root:
    powershell -NoProfile -ExecutionPolicy Bypass -File python\strategy_lab\setup.ps1
  Options:  -Python <path-to-python.exe>   -Recreate (delete and rebuild .venv)
#>
[CmdletBinding()]
param(
    [string]$Python = "",
    [switch]$Recreate
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here "..\..")).Path
$venv = Join-Path $here ".venv"
$venvPython = Join-Path $venv "Scripts\python.exe"
$requirements = Join-Path $here "requirements.txt"

function Fail([string]$message) {
    Write-Host ""
    Write-Host "SETUP FAILED: $message" -ForegroundColor Red
    exit 1
}

# Runs a native command and returns its exit code. Native stderr text is not treated as a PowerShell error
# (Windows PowerShell 5.1 would otherwise stop on pip's harmless notices).
function Invoke-Native([string]$file, [string[]]$arguments) {
    $saved = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        # Out-Host keeps the tool's output on screen and OUT of this function's return value (only the exit code is returned).
        & $file @arguments | Out-Host
        return $LASTEXITCODE
    } finally { $ErrorActionPreference = $saved }
}

function Test-Interpreter([string]$file, [string[]]$prefix) {
    try {
        $saved = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        $text = & $file @prefix -c "import sys; print('%d.%d' % sys.version_info[:2])" 2>$null
        $ErrorActionPreference = $saved
        if ($LASTEXITCODE -ne 0 -or -not $text) { return $null }
        $parts = ("$text").Trim().Split(".")
        if ([int]$parts[0] -gt 3 -or ([int]$parts[0] -eq 3 -and [int]$parts[1] -ge 10)) { return @($file) + $prefix }
    } catch { }
    return $null
}

# 1. Find a suitable Python.
$interpreter = $null
if ($Python) {
    $interpreter = Test-Interpreter $Python @()
    if (-not $interpreter) { Fail "'$Python' is not a working Python 3.10 or newer." }
} else {
    $interpreter = Test-Interpreter "py" @("-3")
    if (-not $interpreter) { $interpreter = Test-Interpreter "python" @() }
    if (-not $interpreter) {
        Fail ("Python 3.10 or newer was not found. Install it from https://www.python.org/downloads/ (tick 'Add python.exe " +
              "to PATH'), or run:  winget install Python.Python.3.12   then open a new terminal and run this script again. " +
              "(The Microsoft Store 'python' shortcut does not count.)")
    }
}
Write-Host "Using Python: $($interpreter -join ' ')"

# 2. Create the environment.
if ($Recreate -and (Test-Path $venv)) {
    if ((Split-Path -Leaf $venv) -ne ".venv") { Fail "Refusing to delete '$venv'." }
    Write-Host "Removing the existing environment..."
    Remove-Item -Recurse -Force $venv
}
if (-not (Test-Path $venvPython)) {
    Write-Host "Creating $venv ..."
    $rest = @()
    if ($interpreter.Count -gt 1) { $rest = $interpreter[1..($interpreter.Count - 1)] }
    $code = Invoke-Native $interpreter[0] ($rest + @("-m", "venv", $venv))
    if ($code -ne 0 -or -not (Test-Path $venvPython)) { Fail "could not create the virtual environment (exit $code)." }
}

# 3. Install the pinned packages into it.
Write-Host "Installing pinned packages from requirements.txt (needs the internet this once)..."
$code = Invoke-Native $venvPython @("-m", "pip", "install", "--disable-pip-version-check", "-r", $requirements)
if ($code -ne 0) {
    Fail ("pip could not install the packages (exit $code). If you are offline, connect once and run this script again; " +
          "if a package cannot be built for your Python version, try -Python with Python 3.12 or 3.13.")
}

# 4. Verify the installed versions match the pins.
$check = @'
import sys
from importlib.metadata import version, PackageNotFoundError
bad = []
for line in open(sys.argv[1], encoding="utf-8"):
    line = line.strip()
    if not line or line.startswith("#"):
        continue
    name, _, want = line.partition("==")
    try:
        have = version(name)
    except PackageNotFoundError:
        have = "not installed"
    print("  %-10s wanted %-8s installed %s" % (name, want, have))
    if have != want:
        bad.append(name)
import streamlit, plotly
sys.exit(1 if bad else 0)
'@
$checkFile = Join-Path $env:TEMP "strategy_lab_verify_pins.py"
Set-Content -Path $checkFile -Value $check -Encoding ASCII
try { $code = Invoke-Native $venvPython @($checkFile, $requirements) } finally { Remove-Item $checkFile -Force -ErrorAction SilentlyContinue }
if ($code -ne 0) { Fail "the installed packages do not match requirements.txt. Run again with -Recreate." }

# 5. Is the C++ replay tool built? (Not built here: that is a separate, one-time step.)
$exe = Join-Path $repo "out\strategy_lab\bin\Release\strategy_lab_replay.exe"
Write-Host ""
Write-Host "Python environment is ready." -ForegroundColor Green
if (Test-Path $exe) {
    Write-Host "Replay tool found: $exe"
} else {
    Write-Host "The C++ replay tool is not built yet. From the repository root run:" -ForegroundColor Yellow
    Write-Host '  cmake -S . -B out/strategy_lab -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=OFF -DTRADING_ENGINE_BUILD_STRATEGY_LAB=ON'
    Write-Host '  cmake --build out/strategy_lab --config Release --target strategy_lab_replay'
}
Write-Host ""
Write-Host "Start the lab with:  powershell -NoProfile -ExecutionPolicy Bypass -File python\strategy_lab\run_lab.ps1"
