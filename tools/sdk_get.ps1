param(
    [ValidateSet('all','latest')][string]$Mode = 'all',
    [string[]]$Modules = @(),
    [string]$Output = 'D:\log_client_test',
    [string]$Endpoint = 'tcp/10.23.2.11:7447',
    [string]$PythonExe = "$PSScriptRoot\..\.venv\Scripts\python.exe"
)
if (-not (Test-Path -LiteralPath $PythonExe -PathType Leaf)) {
    Write-Error 'Python environment missing. Create .venv as described in README.md or pass -PythonExe.'
    exit 1
}
& $PythonExe "$PSScriptRoot\sdk_client.py" --endpoint $Endpoint --mode $Mode --output $Output --modules @Modules
exit $LASTEXITCODE
