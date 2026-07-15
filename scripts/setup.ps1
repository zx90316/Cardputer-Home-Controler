$ErrorActionPreference = "Stop"

$bundledPython = Join-Path $env:USERPROFILE ".cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe"
$python = if (Get-Command py -ErrorAction SilentlyContinue) { "py" } elseif (Test-Path $bundledPython) { $bundledPython } else { "python" }

if (-not (Test-Path ".venv")) {
    if ($python -eq "py") { & py -3.12 -m venv .venv } else { & $python -m venv .venv }
}

& .\.venv\Scripts\python.exe -m pip install --requirement requirements.txt
& .\.venv\Scripts\python.exe -m pytest
