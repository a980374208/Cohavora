param(
    [Parameter(Mandatory=$true)][ValidateSet('start','stop')][string]$Action,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [Parameter(Mandatory=$true)][ValidatePattern('^[A-Za-z0-9][A-Za-z0-9_-]{0,79}$')][string]$Tag
)
$ErrorActionPreference='Stop'
$root=(Resolve-Path -LiteralPath $OutputDirectory).Path
$trace=Join-Path $root "$Tag.etl"
$start=Join-Path $root "$Tag-start.json"
$marker=Join-Path $root "$Tag-$Action.json"
$instance="LiveKitHandle-$Tag"
function Invoke-WprCommand([string[]]$Arguments) {
    # Windows PowerShell treats native stderr as ErrorRecord. WPR writes
    # diagnostics there; preserve them and decide success from its exit code.
    $savedPreference=$ErrorActionPreference
    try {
        $ErrorActionPreference='Continue'
        $messages=& wpr @Arguments 2>&1
        return @{exit_code=$LASTEXITCODE;output=($messages -join "`n")}
    } finally {
        $ErrorActionPreference=$savedPreference
    }
}
if(Test-Path -LiteralPath $marker){throw 'Trace action evidence already exists'}
if(Test-Path -LiteralPath $trace){throw 'Trace already exists'}
if($Action -eq 'start') {
    $result=Invoke-WprCommand @('-start',"${PSScriptRoot}\handle-stacks.wprp!HandleStacks",'-filemode','-instancename',$instance)
} else {
    $owner=Get-Content -LiteralPath $start -Raw | ConvertFrom-Json
    if($owner.exit_code -ne 0 -or $owner.instance -ne $instance){throw 'No successful owned trace start'}
    $result=Invoke-WprCommand @('-stop',$trace,'-instancename',$instance)
}
$code=$result.exit_code
[ordered]@{action=$Action;tag=$Tag;instance=$instance;exit_code=$code;output=$result.output} |
    ConvertTo-Json | Set-Content -LiteralPath $marker -Encoding UTF8
if($code -ne 0){throw "WPR $Action failed: $code"}
