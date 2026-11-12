param(
    [ValidateSet('all','latest')][string]$Mode = 'all',
    [string[]]$Modules = @(),
    [string]$Output = 'D:\codex_log\log-service-received',
    [string]$Endpoint = 'tcp/10.22.69.16:17447'
)
$clientPython = 'D:\codex_log\tools\zenoh-test\Scripts\python.exe'
& $clientPython "$PSScriptRoot\sdk_client.py" --endpoint $Endpoint --mode $Mode --output $Output --modules @Modules
exit $LASTEXITCODE
