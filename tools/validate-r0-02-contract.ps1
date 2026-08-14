[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$contractRoot = Join-Path (Split-Path -Parent $PSScriptRoot) 'docs\validation\r0-02'
$knownCapabilities = @('event.lockByUtc', 'status.timeSync', 'preview.frame.time')
$clockSources = @('PTP_HARDWARE', 'PTP_SOFTWARE', 'NTP', 'OFFSET_MODEL', 'RECEIVE_CLOCK', 'UNKNOWN')
$syncStates = @('SYNCED', 'SYNCING', 'DEGRADED', 'UNSYNCED', 'UNKNOWN')
$triggerSources = @('ALGORITHM', 'MANUAL', 'PLANT_IO', 'EXTERNAL')

function Stop-ContractValidation {
    param([string]$Code, [string]$Message)
    throw [System.InvalidOperationException]::new("$Code|$Message")
}

function Assert-Contract {
    param([bool]$Condition, [string]$Code, [string]$Message)
    if (-not $Condition) {
        Stop-ContractValidation -Code $Code -Message $Message
    }
}

function Read-ContractJson {
    param([string]$Name)
    $path = Join-Path $contractRoot $Name
    $raw = [System.IO.File]::ReadAllText($path, [System.Text.UTF8Encoding]::new($false))
    Assert-Contract -Condition ([System.Text.Encoding]::UTF8.GetByteCount($raw) -le 1MB) `
        -Code 'UPLINK_PROTOCOL_ERROR' -Message "$Name exceeds 1 MiB"
    try {
        return ($raw | ConvertFrom-Json)
    }
    catch {
        Stop-ContractValidation -Code 'UPLINK_PROTOCOL_ERROR' -Message "$Name is not valid JSON"
    }
}

function Assert-Fields {
    param(
        [object]$Value,
        [string[]]$Required,
        [string[]]$Optional = @(),
        [string]$Context
    )
    Assert-Contract -Condition ($null -ne $Value) -Code 'UPLINK_PROTOCOL_ERROR' `
        -Message "$Context must be an object"
    $actual = @($Value.PSObject.Properties.Name)
    foreach ($name in $Required) {
        Assert-Contract -Condition ($actual -ccontains $name) -Code 'UPLINK_PROTOCOL_ERROR' `
            -Message "$Context is missing $name"
    }
    foreach ($name in $actual) {
        Assert-Contract -Condition (($Required -ccontains $name) -or ($Optional -ccontains $name)) `
            -Code 'UPLINK_PROTOCOL_ERROR' -Message "$Context contains unknown field $name"
    }
}

function Assert-Identifier {
    param([object]$Value, [int]$MaximumBytes, [string]$Context)
    Assert-Contract -Condition ($Value -is [string]) -Code 'UPLINK_PROTOCOL_ERROR' `
        -Message "$Context must be a string"
    Assert-Contract -Condition ([System.Text.Encoding]::UTF8.GetByteCount($Value) -ge 1 -and `
        [System.Text.Encoding]::UTF8.GetByteCount($Value) -le $MaximumBytes) `
        -Code 'UPLINK_PROTOCOL_ERROR' -Message "$Context length is invalid"
    Assert-Contract -Condition ($Value -cmatch '^[A-Za-z0-9._-]+$' -and $Value -notmatch '\.\.') `
        -Code 'UPLINK_PROTOCOL_ERROR' -Message "$Context is not path-safe"
}

function Assert-IntegerRange {
    param([object]$Value, [long]$Minimum, [long]$Maximum, [string]$Context)
    $isInteger = $Value -is [byte] -or $Value -is [sbyte] -or $Value -is [int16] -or `
        $Value -is [uint16] -or $Value -is [int32] -or $Value -is [uint32] -or `
        $Value -is [int64]
    Assert-Contract -Condition $isInteger -Code 'UPLINK_PROTOCOL_ERROR' `
        -Message "$Context must be an integer"
    $number = [long]$Value
    Assert-Contract -Condition ($number -ge $Minimum -and $number -le $Maximum) `
        -Code 'UPLINK_PROTOCOL_ERROR' -Message "$Context is outside its range"
}

function Assert-Nanoseconds {
    param([object]$Value, [bool]$Nullable, [bool]$NonNegative, [string]$Context)
    if ($null -eq $Value) {
        Assert-Contract -Condition $Nullable -Code 'UPLINK_PROTOCOL_ERROR' `
            -Message "$Context cannot be null"
        return
    }
    Assert-Contract -Condition ($Value -is [string]) -Code 'UPLINK_PROTOCOL_ERROR' `
        -Message "$Context must be a decimal string"
    Assert-Contract -Condition ($Value -cmatch '^(0|-?[1-9][0-9]{0,18})$') `
        -Code 'UPLINK_PROTOCOL_ERROR' -Message "$Context is not canonical decimal"
    $parsed = 0L
    Assert-Contract -Condition ([long]::TryParse($Value, [Globalization.NumberStyles]::AllowLeadingSign,
        [Globalization.CultureInfo]::InvariantCulture, [ref]$parsed)) `
        -Code 'UPLINK_PROTOCOL_ERROR' -Message "$Context exceeds int64"
    Assert-Contract -Condition (-not $NonNegative -or $parsed -ge 0) `
        -Code 'UPLINK_PROTOCOL_ERROR' -Message "$Context must be non-negative"
}

function Assert-Rfc3339Milliseconds {
    param([object]$Value, [string]$Context)
    Assert-Contract -Condition ($Value -is [string] -and
        $Value -cmatch '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$') `
        -Code 'UPLINK_PROTOCOL_ERROR' -Message "$Context must be UTC RFC 3339 milliseconds"
}

function Assert-Envelope {
    param([object]$Value, [string]$ExpectedType)
    Assert-Fields -Value $Value -Required @('protocolVersion', 'messageType', 'messageId',
        'machineId', 'sequence', 'timestamp', 'payload') -Optional @('extensions') -Context 'envelope'
    if ($Value.protocolVersion -ne 1) {
        Stop-ContractValidation -Code 'UPLINK_PROTOCOL_VERSION_UNSUPPORTED' `
            -Message 'protocolVersion must be 1'
    }
    Assert-Contract -Condition ($Value.messageType -ceq $ExpectedType) -Code 'UPLINK_PROTOCOL_ERROR' `
        -Message "messageType must be $ExpectedType"
    Assert-Identifier -Value $Value.messageId -MaximumBytes 128 -Context 'messageId'
    Assert-Identifier -Value $Value.machineId -MaximumBytes 64 -Context 'machineId'
    Assert-IntegerRange -Value $Value.sequence -Minimum 0 -Maximum ([long]::MaxValue) -Context 'sequence'
    Assert-Rfc3339Milliseconds -Value $Value.timestamp -Context 'timestamp'
}

function Assert-SessionPair {
    param([object]$Request, [object]$Response)
    Assert-Fields -Value $Request -Required @('requestId', 'machineId', 'productionLineId',
        'softwareVersion', 'supportedProtocolVersions', 'capabilities') -Optional @('extensions') `
        -Context 'session request'
    Assert-Identifier $Request.requestId 128 'requestId'
    Assert-Identifier $Request.machineId 64 'machineId'
    Assert-Identifier $Request.productionLineId 64 'productionLineId'
    Assert-Contract -Condition (@($Request.supportedProtocolVersions) -contains 1) `
        -Code 'UPLINK_PROTOCOL_VERSION_UNSUPPORTED' -Message 'session request does not offer v1'
    $offered = @($Request.capabilities)
    foreach ($capability in $offered) {
        Assert-Identifier $capability 128 'capability'
    }

    Assert-Fields -Value $Response -Required @('protocolVersion', 'requestId', 'sessionId',
        'machineId', 'serverTime', 'heartbeatSeconds', 'webSocketUrl', 'extensions') `
        -Context 'session response'
    Assert-Contract -Condition ($Response.protocolVersion -eq 1) `
        -Code 'UPLINK_PROTOCOL_VERSION_UNSUPPORTED' -Message 'session response selected unknown version'
    Assert-Rfc3339Milliseconds $Response.serverTime 'serverTime'
    Assert-Fields -Value $Response.extensions -Required @('paperbreak') -Context 'extensions'
    Assert-Fields -Value $Response.extensions.paperbreak `
        -Required @('capabilityContractVersion', 'acceptedCapabilities') `
        -Context 'extensions.paperbreak'
    Assert-Contract -Condition ($Response.extensions.paperbreak.capabilityContractVersion -eq 1) `
        -Code 'UPLINK_PROTOCOL_ERROR' -Message 'unknown capability contract version'
    foreach ($capability in @($Response.extensions.paperbreak.acceptedCapabilities)) {
        Assert-Contract -Condition (($knownCapabilities -ccontains $capability) -and
            ($offered -ccontains $capability)) -Code 'UPLINK_PROTOCOL_ERROR' `
            -Message "invalid accepted capability $capability"
    }
}

function Assert-TriggerFields {
    param([object]$Value, [bool]$IncludeNotificationFields, [string]$Context)
    $required = @('eventId', 'triggerTimestampUtcNs', 'triggerMachineId', 'triggerCameraId',
        'triggerSource', 'preEventMs', 'postEventMs')
    if ($IncludeNotificationFields) {
        $required += @('code', 'eventType', 'eventLevel', 'syncState', 'uncertaintyNs',
            'clockModelRevision', 'configRevision')
    }
    Assert-Fields -Value $Value -Required $required -Optional @('extensions') -Context $Context
    Assert-Identifier $Value.eventId 128 "$Context.eventId"
    Assert-Identifier $Value.triggerMachineId 64 "$Context.triggerMachineId"
    Assert-Identifier $Value.triggerCameraId 64 "$Context.triggerCameraId"
    Assert-Nanoseconds $Value.triggerTimestampUtcNs $false $false "$Context.triggerTimestampUtcNs"
    Assert-Contract -Condition ($triggerSources -ccontains $Value.triggerSource) `
        -Code 'UPLINK_PROTOCOL_ERROR' -Message "$Context.triggerSource is unknown"
    Assert-IntegerRange $Value.preEventMs 0 600000 "$Context.preEventMs"
    Assert-IntegerRange $Value.postEventMs 0 600000 "$Context.postEventMs"
    if ($IncludeNotificationFields) {
        Assert-Contract -Condition ($Value.code -ceq 'BREAK_EVENT_TRIGGERED' -and
            $Value.eventType -ceq 'PAPER_BREAK' -and $Value.eventLevel -ceq 'ALARM') `
            -Code 'UPLINK_PROTOCOL_ERROR' -Message "$Context fixed values are invalid"
        Assert-Contract -Condition ($syncStates -ccontains $Value.syncState) `
            -Code 'UPLINK_PROTOCOL_ERROR' -Message "$Context.syncState is unknown"
        Assert-Nanoseconds $Value.uncertaintyNs $true $true "$Context.uncertaintyNs"
        Assert-IntegerRange $Value.clockModelRevision 0 ([long]::MaxValue) "$Context.clockModelRevision"
        Assert-IntegerRange $Value.configRevision 0 ([long]::MaxValue) "$Context.configRevision"
    }
}

function Assert-ClockSyncSnapshot {
    param([object]$Value, [string]$Context)
    Assert-Fields -Value $Value -Required @('available', 'currentUtcNs', 'clockSource', 'offsetNs',
        'offsetAvailable', 'uncertaintyNs', 'uncertaintyAvailable', 'maximumObservedOffsetNs',
        'maximumObservedOffsetAvailable', 'lastSynchronizedUtcNs',
        'lastSynchronizedUtcNsAvailable', 'syncState', 'grandmasterIdentity',
        'grandmasterAvailable', 'modelRevision', 'lastErrorCode') -Context $Context
    Assert-Contract -Condition ($Value.available -is [bool]) -Code 'UPLINK_PROTOCOL_ERROR' `
        -Message "$Context.available must be bool"
    Assert-Nanoseconds $Value.currentUtcNs $true $false "$Context.currentUtcNs"
    Assert-Nanoseconds $Value.offsetNs $true $false "$Context.offsetNs"
    Assert-Nanoseconds $Value.uncertaintyNs $true $true "$Context.uncertaintyNs"
    Assert-Nanoseconds $Value.maximumObservedOffsetNs $true $true "$Context.maximumObservedOffsetNs"
    Assert-Nanoseconds $Value.lastSynchronizedUtcNs $true $false "$Context.lastSynchronizedUtcNs"
    foreach ($pair in @(@('offsetAvailable', 'offsetNs'), @('uncertaintyAvailable', 'uncertaintyNs'),
        @('maximumObservedOffsetAvailable', 'maximumObservedOffsetNs'),
        @('lastSynchronizedUtcNsAvailable', 'lastSynchronizedUtcNs'),
        @('grandmasterAvailable', 'grandmasterIdentity'))) {
        $flag = $Value.($pair[0])
        Assert-Contract -Condition ($flag -is [bool] -and $flag -eq ($null -ne $Value.($pair[1]))) `
            -Code 'UPLINK_PROTOCOL_ERROR' -Message "$Context.$($pair[0]) is inconsistent"
    }
    Assert-Contract -Condition ($clockSources -ccontains $Value.clockSource) `
        -Code 'UPLINK_PROTOCOL_ERROR' -Message "$Context.clockSource is unknown"
    Assert-Contract -Condition ($syncStates -ccontains $Value.syncState) `
        -Code 'UPLINK_PROTOCOL_ERROR' -Message "$Context.syncState is unknown"
    Assert-IntegerRange $Value.modelRevision 0 ([long]::MaxValue) "$Context.modelRevision"
    if (-not $Value.available) {
        Assert-Contract -Condition ($null -eq $Value.currentUtcNs -and
            $Value.clockSource -ceq 'UNKNOWN' -and $Value.syncState -ceq 'UNKNOWN' -and
            $Value.modelRevision -eq 0) -Code 'UPLINK_PROTOCOL_ERROR' `
            -Message "$Context unavailable snapshot is inconsistent"
    }
}

function Assert-LastFrameTime {
    param([object]$Value, [string]$Context)
    Assert-Fields -Value $Value -Required @('receivedUtcNs', 'correctedCaptureUtcNs',
        'correctedCaptureUtcNsAvailable', 'syncState', 'uncertaintyNs',
        'uncertaintyAvailable', 'clockModelRevision') -Context $Context
    Assert-Nanoseconds $Value.receivedUtcNs $true $false "$Context.receivedUtcNs"
    Assert-Nanoseconds $Value.correctedCaptureUtcNs $true $false "$Context.correctedCaptureUtcNs"
    Assert-Nanoseconds $Value.uncertaintyNs $true $true "$Context.uncertaintyNs"
    Assert-Contract -Condition ($Value.correctedCaptureUtcNsAvailable -is [bool] -and
        $Value.correctedCaptureUtcNsAvailable -eq ($null -ne $Value.correctedCaptureUtcNs)) `
        -Code 'UPLINK_PROTOCOL_ERROR' -Message "$Context corrected time availability is inconsistent"
    Assert-Contract -Condition ($Value.uncertaintyAvailable -is [bool] -and
        $Value.uncertaintyAvailable -eq ($null -ne $Value.uncertaintyNs)) `
        -Code 'UPLINK_PROTOCOL_ERROR' -Message "$Context uncertainty availability is inconsistent"
    Assert-Contract -Condition ($syncStates -ccontains $Value.syncState) `
        -Code 'UPLINK_PROTOCOL_ERROR' -Message "$Context.syncState is unknown"
    Assert-IntegerRange $Value.clockModelRevision 0 ([long]::MaxValue) "$Context.clockModelRevision"
}

function Assert-EventLockAck {
    param([object]$Value)
    Assert-Fields -Value $Value -Required @('eventId', 'machineId', 'duplicate', 'lockStatus',
        'requestedStartUtcNs', 'requestedEndUtcNs', 'actualStartUtcNs', 'actualEndUtcNs',
        'syncState', 'uncertaintyNs', 'clockModelRevision', 'cameras') -Optional @('extensions') `
        -Context 'EventLockAck'
    Assert-Identifier $Value.eventId 128 'EventLockAck.eventId'
    Assert-Identifier $Value.machineId 64 'EventLockAck.machineId'
    Assert-Contract -Condition ($Value.duplicate -is [bool]) -Code 'UPLINK_PROTOCOL_ERROR' `
        -Message 'EventLockAck.duplicate must be bool'
    Assert-Contract -Condition (@('COMPLETE', 'PARTIAL', 'FAILED') -ccontains $Value.lockStatus) `
        -Code 'UPLINK_PROTOCOL_ERROR' -Message 'EventLockAck.lockStatus is unknown'
    Assert-Nanoseconds $Value.requestedStartUtcNs $false $false 'requestedStartUtcNs'
    Assert-Nanoseconds $Value.requestedEndUtcNs $false $false 'requestedEndUtcNs'
    Assert-Nanoseconds $Value.actualStartUtcNs $true $false 'actualStartUtcNs'
    Assert-Nanoseconds $Value.actualEndUtcNs $true $false 'actualEndUtcNs'
    Assert-Nanoseconds $Value.uncertaintyNs $true $true 'EventLockAck.uncertaintyNs'
    Assert-Contract -Condition ($syncStates -ccontains $Value.syncState) `
        -Code 'UPLINK_PROTOCOL_ERROR' -Message 'EventLockAck.syncState is unknown'
    Assert-IntegerRange $Value.clockModelRevision 0 ([long]::MaxValue) 'clockModelRevision'
    $cameras = @($Value.cameras)
    Assert-Contract -Condition ($cameras.Count -le 6) -Code 'UPLINK_PROTOCOL_ERROR' `
        -Message 'EventLockAck has more than six cameras'
    $cameraIds = @{}
    foreach ($camera in $cameras) {
        Assert-Fields -Value $camera -Required @('cameraId', 'status', 'frameCount',
            'firstCaptureUtcNs', 'lastCaptureUtcNs', 'sequenceGaps', 'errorCode') `
            -Context 'EventLockAck.camera'
        Assert-Identifier $camera.cameraId 64 'EventLockAck.camera.cameraId'
        Assert-Contract -Condition (-not $cameraIds.ContainsKey($camera.cameraId)) `
            -Code 'UPLINK_PROTOCOL_ERROR' -Message 'EventLockAck has duplicate cameraId'
        $cameraIds[$camera.cameraId] = $true
        Assert-Contract -Condition (@('LOCKED', 'PARTIAL', 'FAILED') -ccontains $camera.status) `
            -Code 'UPLINK_PROTOCOL_ERROR' -Message 'camera lock status is unknown'
        Assert-IntegerRange $camera.frameCount 0 ([long]::MaxValue) 'camera.frameCount'
        Assert-IntegerRange $camera.sequenceGaps 0 ([long]::MaxValue) 'camera.sequenceGaps'
        Assert-Nanoseconds $camera.firstCaptureUtcNs $true $false 'camera.firstCaptureUtcNs'
        Assert-Nanoseconds $camera.lastCaptureUtcNs $true $false 'camera.lastCaptureUtcNs'
        Assert-Contract -Condition ($null -eq $camera.errorCode -or $camera.errorCode -is [string]) `
            -Code 'UPLINK_PROTOCOL_ERROR' -Message 'camera.errorCode must be string or null'
    }
    if ($Value.lockStatus -ceq 'COMPLETE') {
        Assert-Contract -Condition ($Value.syncState -ceq 'SYNCED' -and
            $null -ne $Value.actualStartUtcNs -and $null -ne $Value.actualEndUtcNs -and
            @($cameras | Where-Object { $_.status -cne 'LOCKED' -or $_.sequenceGaps -ne 0 }).Count -eq 0) `
            -Code 'UPLINK_PROTOCOL_ERROR' -Message 'COMPLETE aggregate violates strict conditions'
    }
}

function Assert-PreviewHeader {
    param([object]$Value)
    Assert-Fields -Value $Value -Required @('protocolVersion', 'messageType', 'messageId',
        'machineId', 'cameraId', 'sequence', 'timestamp', 'jpegBytes',
        'correctedCaptureUtcNs', 'correctedCaptureUtcNsAvailable', 'syncState',
        'uncertaintyNs', 'uncertaintyAvailable', 'clockModelRevision') -Optional @('extensions') `
        -Context 'preview header'
    Assert-Contract -Condition ($Value.protocolVersion -eq 1 -and
        $Value.messageType -ceq 'preview.frame') -Code 'UPLINK_PROTOCOL_ERROR' `
        -Message 'preview version/type is invalid'
    Assert-Identifier $Value.messageId 128 'preview.messageId'
    Assert-Identifier $Value.machineId 64 'preview.machineId'
    Assert-Identifier $Value.cameraId 64 'preview.cameraId'
    Assert-IntegerRange $Value.sequence 0 ([long]::MaxValue) 'preview.sequence'
    Assert-IntegerRange $Value.jpegBytes 0 (2MB) 'preview.jpegBytes'
    Assert-Rfc3339Milliseconds $Value.timestamp 'preview.timestamp'
    Assert-Nanoseconds $Value.correctedCaptureUtcNs $true $false 'preview.correctedCaptureUtcNs'
    Assert-Nanoseconds $Value.uncertaintyNs $true $true 'preview.uncertaintyNs'
    Assert-Contract -Condition ($Value.correctedCaptureUtcNsAvailable -is [bool] -and
        $Value.correctedCaptureUtcNsAvailable -eq ($null -ne $Value.correctedCaptureUtcNs)) `
        -Code 'UPLINK_PROTOCOL_ERROR' -Message 'preview corrected time availability is inconsistent'
    Assert-Contract -Condition ($Value.uncertaintyAvailable -is [bool] -and
        $Value.uncertaintyAvailable -eq ($null -ne $Value.uncertaintyNs)) `
        -Code 'UPLINK_PROTOCOL_ERROR' -Message 'preview uncertainty availability is inconsistent'
    Assert-Contract -Condition ($syncStates -ccontains $Value.syncState) `
        -Code 'UPLINK_PROTOCOL_ERROR' -Message 'preview.syncState is unknown'
    Assert-IntegerRange $Value.clockModelRevision 0 ([long]::MaxValue) 'preview.clockModelRevision'
}

function Assert-Rejected {
    param([scriptblock]$Action, [string]$ExpectedCode, [string]$Context)
    try {
        & $Action
    }
    catch {
        $actual = $_.Exception.Message.Split('|', 2)[0]
        Assert-Contract -Condition ($actual -ceq $ExpectedCode) -Code 'VALIDATION_SCRIPT_ERROR' `
            -Message "$Context returned $actual instead of $ExpectedCode"
        return
    }
    Stop-ContractValidation -Code 'VALIDATION_SCRIPT_ERROR' `
        -Message "$Context was accepted but must return $ExpectedCode"
}

$sessionRequest = Read-ContractJson 'strict-session-request.json'
$sessionResponse = Read-ContractJson 'strict-session-response.json'
Assert-SessionPair $sessionRequest $sessionResponse

$notification = Read-ContractJson 'break-event-triggered.json'
Assert-Envelope $notification 'alarm'
Assert-TriggerFields $notification.payload $true 'BREAK_EVENT_TRIGGERED'

$command = Read-ContractJson 'event-lock-command.json'
Assert-Envelope $command 'command'
Assert-Fields $command.payload @('commandId', 'commandType', 'deadline', 'body') `
    @('operatorConfirmed', 'extensions') 'event.lockByUtc payload'
Assert-Identifier $command.payload.commandId 128 'commandId'
Assert-Contract -Condition ($command.payload.commandType -ceq 'event.lockByUtc') `
    -Code 'UPLINK_PROTOCOL_ERROR' -Message 'commandType must be event.lockByUtc'
Assert-Rfc3339Milliseconds $command.payload.deadline 'deadline'
Assert-TriggerFields $command.payload.body $false 'event.lockByUtc body'

$ackEnvelope = Read-ContractJson 'event-lock-ack.json'
Assert-Envelope $ackEnvelope 'command.result'
Assert-Fields $ackEnvelope.payload @('commandId', 'success', 'result') @('extensions') `
    'command.result payload'
Assert-Contract -Condition ($ackEnvelope.payload.success -eq $true) -Code 'UPLINK_PROTOCOL_ERROR' `
    -Message 'sample command.result must be successful'
Assert-EventLockAck $ackEnvelope.payload.result

$status = Read-ContractJson 'status-time.json'
Assert-Envelope $status 'status'
Assert-Fields $status.payload @('timeSync', 'cameras') @('extensions') 'status payload sample'
Assert-ClockSyncSnapshot $status.payload.timeSync 'status.timeSync'
Assert-Contract -Condition (@($status.payload.cameras).Count -le 4) -Code 'UPLINK_PROTOCOL_ERROR' `
    -Message 'status sample has more than four cameras'
foreach ($camera in @($status.payload.cameras)) {
    Assert-Fields $camera @('cameraId', 'timeSync', 'lastFrameTime') @('extensions') 'status.camera sample'
    Assert-Identifier $camera.cameraId 64 'status.camera.cameraId'
    Assert-ClockSyncSnapshot $camera.timeSync "status.camera[$($camera.cameraId)].timeSync"
    Assert-LastFrameTime $camera.lastFrameTime "status.camera[$($camera.cameraId)].lastFrameTime"
}

Assert-PreviewHeader (Read-ContractJson 'preview-time-header.json')

Assert-Rejected -ExpectedCode 'UPLINK_PROTOCOL_ERROR' -Context 'unknown lock body field' -Action {
    Assert-TriggerFields (Read-ContractJson 'invalid-unknown-field.json') $false 'invalid lock body'
}
Assert-Rejected -ExpectedCode 'UPLINK_PROTOCOL_VERSION_UNSUPPORTED' -Context 'unknown protocol version' -Action {
    Assert-Envelope (Read-ContractJson 'invalid-version.json') 'status'
}
Assert-Rejected -ExpectedCode 'UPLINK_PROTOCOL_ERROR' -Context 'invalid accepted capability' -Action {
    Assert-SessionPair $sessionRequest (Read-ContractJson 'invalid-accepted-capability.json')
}

Write-Output 'R0-02 contract samples are valid; rejection vectors returned stable business codes.'
