param(
    [Parameter(Mandatory=$true)][string]$PreparedDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateRange(1,172800)][int]$MinimumRemainingSeconds=900,
    [switch]$CreateNew
)
$ErrorActionPreference='Stop'
$setup=Get-Content (Join-Path $PreparedDirectory 'setup.json') -Raw | ConvertFrom-Json
if ($setup.status -ne 'PREPARED' -or $setup.meeting_id -cnotmatch '^[0-9]{9}$') { throw 'PREPARED_MEETING_INVALID' }
$endpoint=[Uri]$setup.service_url
$target=Get-Content (Join-Path $PSScriptRoot 'product_aliyun_target.json') -Raw | ConvertFrom-Json
if ($endpoint.AbsoluteUri.TrimEnd('/') -ne $target.service_url) { throw 'TEST_SERVICE_UNEXPECTED' }
$secret=(Get-Content (Join-Path $PreparedDirectory 'password.dpapi') -Raw).Trim() | ConvertTo-SecureString
$password=[Net.NetworkCredential]::new('', $secret).Password
$token=$null
function Request-MeetingApi([string]$Route, $Body) {
    $headers=@{operationID=[guid]::NewGuid().ToString('N')}
    if ($token) {$headers.token=$token}
    try {
        $response=Invoke-RestMethod -Uri ($setup.service_url.TrimEnd('/')+$Route) -Method Post `
            -Headers $headers -Body ($Body | ConvertTo-Json -Depth 8 -Compress) `
            -ContentType 'application/json' -TimeoutSec 20 -MaximumRedirection 0
    } catch { throw "MEETING_API_TRANSPORT_FAILED: $Route" }
    if ($null -eq $response.errCode -or $response.errCode -ne 0) {
        # Never put the raw response, credentials, tokens or business text in evidence.
        throw "MEETING_API_REJECTED: $Route code=$($response.errCode)"
    }
    return $response.data
}
try {
    $login=Request-MeetingApi '/user/login' @{account=$setup.account;password=$password}
    $password=$null
    $token=$login.token
    if (!$token -or !$login.userID) {throw 'MEETING_LOGIN_IDENTITY_MISSING'}
    $old=Request-MeetingApi '/meeting/get_meeting' @{userID=$login.userID;meetingID=$setup.meeting_id}
    $oldInfo=$old.meetingDetail.info
    $now=[DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    $oldEnd=[long]$oldInfo.creatorDefinedMeeting.scheduledTime+[long]$oldInfo.creatorDefinedMeeting.meetingDuration
    if ($CreateNew) {
        if (Test-Path -LiteralPath $OutputDirectory) {throw 'PREPARED_OUTPUT_MUST_BE_NEW'}
        $duration=[Math]::Max(259200,$MinimumRemainingSeconds+3600)
        $created=Request-MeetingApi '/meeting/create_immediate_meeting' @{
            creatorUserID=$login.userID
            creatorDefinedMeetingInfo=@{title='UIA external acceptance';scheduledTime=$now;meetingDuration=$duration;password=''}
            setting=@{canParticipantsEnableCamera=$true;canParticipantsShareScreen=$true;canParticipantsUnmuteMicrophone=$true;disableCameraOnJoin=$false;disableMicrophoneOnJoin=$false}
        }
        $meetingId=$created.detail.info.systemGenerated.meetingID
        if ($meetingId -cnotmatch '^[0-9]{9}$') {throw 'CREATED_MEETING_ID_INVALID'}
    } else {$meetingId=$setup.meeting_id}
    $detail=Request-MeetingApi '/meeting/get_meeting' @{userID=$login.userID;meetingID=$meetingId}
    $info=$detail.meetingDetail.info
    if ($CreateNew) {
        $actualDuration=$info.creatorDefinedMeeting.meetingDuration
        # Int32/Int64 JSON integers are exact and intrinsically within Int64 range.
        if ($actualDuration -isnot [int32] -and $actualDuration -isnot [int64]) {
            throw 'CREATED_MEETING_DURATION_INVALID'
        }
        if ($actualDuration -lt 259200) {throw 'CREATED_MEETING_DURATION_TOO_SHORT'}
    }
    $end=[long]$info.creatorDefinedMeeting.scheduledTime+[long]$info.creatorDefinedMeeting.meetingDuration
    $remaining=$end-[DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    if ($info.systemGenerated.meetingID -ne $meetingId -or $info.systemGenerated.creatorUserID -ne $login.userID) {throw 'MEETING_FIXTURE_OWNERSHIP_MISMATCH'}
    if ($remaining -lt $MinimumRemainingSeconds) {throw "MEETING_VALIDITY_TOO_SHORT: remaining=$remaining required=$MinimumRemainingSeconds"}
    if ($CreateNew) {
        $null=New-Item -ItemType Directory -Path $OutputDirectory
        Copy-Item -LiteralPath (Join-Path $PreparedDirectory 'password.dpapi') -Destination (Join-Path $OutputDirectory 'password.dpapi')
        @{meeting_id=$meetingId;status='PREPARED';created_utc=[DateTime]::UtcNow.ToString('o');service_url=$setup.service_url;account=$setup.account} |
            ConvertTo-Json | Set-Content (Join-Path $OutputDirectory 'setup.json') -Encoding UTF8
    }
    $evidence=[ordered]@{status='VERIFIED';meeting_id=$meetingId;verified_utc=[DateTime]::UtcNow.ToString('o');
        remaining_seconds=$remaining;required_seconds=$MinimumRemainingSeconds;expires_unix_seconds=$end;
        previous_meeting_id=$setup.meeting_id;previous_expires_unix_seconds=$oldEnd;previous_remaining_seconds=($oldEnd-$now);
        owner_verified=$true;created_new=[bool]$CreateNew}
    if ($CreateNew) {
        $evidence.requested_duration_seconds=$duration
        $evidence.actual_duration_seconds=$actualDuration
    }
    $evidence | ConvertTo-Json | Set-Content (Join-Path $OutputDirectory 'meeting-preflight.json') -Encoding UTF8
    $evidence | ConvertTo-Json -Compress
} finally {$password=$null;$token=$null;$login=$null;$created=$null;$secret=$null}
