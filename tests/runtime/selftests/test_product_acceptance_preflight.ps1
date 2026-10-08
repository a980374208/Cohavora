param([string]$OutputDirectory="$PSScriptRoot/../../../out/b14-repair-20261007")
$ErrorActionPreference='Stop'
$domain=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../tools/product_acceptance'))
$asts=@{}
foreach($name in @('run_b14_acceptance.ps1','continue_product_acceptance.ps1','product_meeting_fixture.ps1')) {
    $tokens=$null;$parseErrors=$null
    $asts[$name]=[Management.Automation.Language.Parser]::ParseFile((Join-Path $domain $name),[ref]$tokens,[ref]$parseErrors)
    if($parseErrors.Count){throw "PARSER_FAILED: $name"}
}
# Execute the real meeting transaction AST with only its network endpoint stubbed.
# No credential file is decrypted, no API is called, and no product is launched.
$transaction=@($asts['product_meeting_fixture.ps1'].EndBlock.Statements | Where-Object {$_ -is [Management.Automation.Language.TryStatementAst]})
if($transaction.Count -ne 1){throw 'MEETING_TRANSACTION_NOT_UNIQUE'}
$body=[scriptblock]::Create($transaction[0].Extent.Text)
$ownedRoot=[IO.Path]::GetFullPath((Join-Path $OutputDirectory ('preflight-'+[guid]::NewGuid().ToString('N'))))
$null=[IO.Directory]::CreateDirectory($ownedRoot)
$PreparedDirectory=Join-Path $ownedRoot 'prepared'
$null=[IO.Directory]::CreateDirectory($PreparedDirectory)
[IO.File]::WriteAllText((Join-Path $PreparedDirectory 'password.dpapi'),'inert placeholder, never decrypted')
$setup=[pscustomobject]@{service_url='https://fixture.invalid';account='fixture';meeting_id='111111111'}
$CreateNew=$true
$script:requestedDuration=$null
$script:fixtureActualDuration=259200
$script:createdCount=0
function Request-MeetingApi([string]$Route,$Body) {
    switch($Route) {
        '/user/login' { return [pscustomobject]@{token='inert';userID='fixture-owner'} }
        '/meeting/create_immediate_meeting' {
            $script:requestedDuration=$Body.creatorDefinedMeetingInfo.meetingDuration
            $script:createdCount++
            return [pscustomobject]@{detail=@{info=@{systemGenerated=@{meetingID='222222222'}}}}
        }
        '/meeting/get_meeting' {
            return [pscustomobject]@{meetingDetail=@{info=@{
                creatorDefinedMeeting=@{scheduledTime=[DateTimeOffset]::UtcNow.ToUnixTimeSeconds();meetingDuration=$script:fixtureActualDuration}
                systemGenerated=@{meetingID=$Body.meetingID;creatorUserID='fixture-owner'}
            }}}
        }
        default {throw "UNEXPECTED_FIXTURE_ROUTE: $Route"}
    }
}
$cases=[Collections.Generic.List[object]]::new()
function Check-MeetingDuration([string]$Name,[int]$MinimumRemainingSeconds,[int]$Actual,[string]$Expected='') {
    $script:fixtureActualDuration=$Actual
    $script:requestedDuration=$null
    $OutputDirectory=Join-Path $ownedRoot $Name
    $failure=''
    try {$null=& $body} catch {$failure=$_.Exception.Message}
    if($failure -cne $Expected){throw "CASE_FAILED: $Name expected=$Expected actual=$failure"}
    if($script:requestedDuration -ne 259200){throw "THREE_DAY_REQUEST_CHANGED: $Name"}
    if(!$Expected) {
        $proof=Get-Content (Join-Path $OutputDirectory 'meeting-preflight.json') -Raw | ConvertFrom-Json
        if($proof.status -ne 'VERIFIED' -or $proof.requested_duration_seconds -ne 259200 -or $proof.actual_duration_seconds -ne $Actual){throw "MEETING_PROOF_CHANGED: $Name"}
    } elseif(Test-Path -LiteralPath $OutputDirectory) {throw 'REJECTED_MEETING_OUTPUT_WRITTEN'}
    $cases.Add(@{name=$Name;status='PASS';requested_duration_seconds=$script:requestedDuration;actual_duration_seconds=$Actual;rejection=$failure})
}
Check-MeetingDuration 'minimum-window' 900 259200
Check-MeetingDuration 'maximum-window' 172800 259200
Check-MeetingDuration 'server-shortened-duration' 900 259199 'CREATED_MEETING_DURATION_TOO_SHORT'
if($script:createdCount -ne 3){throw 'FIXTURE_CALL_COUNT_INVALID'}
$hashes=[ordered]@{}
foreach($name in $asts.Keys){$hashes[$name]=(Get-FileHash (Join-Path $domain $name) -Algorithm SHA256).Hash.ToLowerInvariant()}
$hashes['test_product_acceptance_preflight.ps1']=(Get-FileHash $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()
$evidence=[ordered]@{schema=1;utc=[DateTime]::UtcNow.ToString('o');status='PASS';parser='PASS';cases=$cases;source_hashes=$hashes;
    scope='Offline actual meeting transaction AST with inert transport; no real meeting, credential decryption, desktop, product or server';qualification_credit=0}
$record=Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) 'meeting-duration-preflight-verification.json'
$evidence|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $record -Encoding UTF8
Write-Output $record
