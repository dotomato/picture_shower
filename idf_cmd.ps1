
# ESP-IDF command wrapper script
# Usage: powershell -ExecutionPolicy Bypass -File idf_cmd.ps1 <idf.py arguments>
# Example: powershell -ExecutionPolicy Bypass -File idf_cmd.ps1 build
#          powershell -ExecutionPolicy Bypass -File idf_cmd.ps1 size-files

param(
    [Parameter(ValueFromRemainingArguments=$true)]
    [string[]]$IdfArgs
)

# Source the IDF environment profile (suppress non-critical errors from profile)
$ErrorActionPreference = "SilentlyContinue"
. 'C:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1'
$ErrorActionPreference = "Continue"

# Define paths with proper quoting
$PythonExe = "C:\Espressif\tools\python\v5.5.4\venv\Scripts\python.exe"
$IdfPy = "D:\Program Files (x86)\.espressif\v5.5.4\esp-idf\tools\idf.py"

# Call idf.py with all passed arguments
if ($IdfArgs.Count -gt 0) {
    & $PythonExe $IdfPy @IdfArgs
} else {
    & $PythonExe $IdfPy --help
}

exit $LASTEXITCODE
