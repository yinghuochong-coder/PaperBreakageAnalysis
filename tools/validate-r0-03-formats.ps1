[CmdletBinding()]
param(
    [switch]$Regenerate
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$contractRoot = Join-Path $repositoryRoot 'docs\validation\r0-03'
$goldenRoot = Join-Path $contractRoot 'golden'
$formatContractPath = Join-Path $contractRoot 'pbnvme3-format.json'
$manifestContractPath = Join-Path $contractRoot 'manifest-v4-fields.json'

$script:HeaderBytes = 4096
$script:IndexEntryBytes = 160
$script:FooterBytes = 4096
$script:AlignmentBytes = 4096
$script:MaximumIndexCapacity = 4096
$script:MaximumFrameBytes = 134217728
$script:MaximumBlockBytes = 68719476736L
$script:MaximumManifestBytes = 8388608
$script:KnownPixelFormats = @(1, 2, 3, 4)
$script:KnownClockSources = @(0, 1, 2, 3, 4, 5)
$script:KnownSyncStates = @(0, 1, 2, 3, 4)
$script:RuntimeInstanceId = '019fff90-3082-74b2-97cd-c9a4a6eccd3b'
$script:RuntimeInstanceHash = 0x1571da0205be1395L

if (-not ('PaperBreak.R003.Crc32C' -as [type])) {
    Add-Type -TypeDefinition @'
namespace PaperBreak.R003
{
    public static class Crc32C
    {
        public static uint Compute(byte[] data, int offset, int count,
                                   int zeroOffset, int zeroCount)
        {
            uint crc = 0xffffffffU;
            int end = checked(offset + count);
            for (int index = offset; index < end; ++index)
            {
                byte value = index >= zeroOffset && index < zeroOffset + zeroCount
                    ? (byte)0 : data[index];
                crc ^= value;
                for (int bit = 0; bit < 8; ++bit)
                    crc = (crc & 1U) != 0U ? (crc >> 1) ^ 0x82f63b78U : crc >> 1;
            }
            return crc ^ 0xffffffffU;
        }
    }
}
'@
}

function Stop-FormatValidation {
    param([string]$Code, [string]$Message)
    throw [System.InvalidOperationException]::new("$Code|$Message")
}

function Assert-Format {
    param([bool]$Condition, [string]$Code, [string]$Message)
    if (-not $Condition) {
        Stop-FormatValidation -Code $Code -Message $Message
    }
}

function Get-Crc32C {
    param(
        [byte[]]$Bytes,
        [int]$Offset = 0,
        [int]$Count = -1,
        [int]$ZeroOffset = -1,
        [int]$ZeroCount = 0
    )
    if ($Count -lt 0) {
        $Count = $Bytes.Length - $Offset
    }
    return [PaperBreak.R003.Crc32C]::Compute($Bytes, $Offset, $Count,
                                             $ZeroOffset, $ZeroCount)
}

function Get-Sha256Text {
    param([byte[]]$Bytes)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        $digest = $algorithm.ComputeHash($Bytes)
    }
    finally {
        $algorithm.Dispose()
    }
    return 'sha256:' + ([BitConverter]::ToString($digest).Replace('-', '').ToLowerInvariant())
}

function Copy-LittleEndian {
    param([byte[]]$Bytes, [int]$Offset, [byte[]]$Value)
    [Array]::Copy($Value, 0, $Bytes, $Offset, $Value.Length)
}

function Set-U16 { param([byte[]]$Bytes, [int]$Offset, [uint16]$Value)
    Copy-LittleEndian $Bytes $Offset ([BitConverter]::GetBytes($Value))
}
function Set-U32 { param([byte[]]$Bytes, [int]$Offset, [uint32]$Value)
    Copy-LittleEndian $Bytes $Offset ([BitConverter]::GetBytes($Value))
}
function Set-U64 { param([byte[]]$Bytes, [int]$Offset, [uint64]$Value)
    Copy-LittleEndian $Bytes $Offset ([BitConverter]::GetBytes($Value))
}
function Set-I64 { param([byte[]]$Bytes, [int]$Offset, [int64]$Value)
    Copy-LittleEndian $Bytes $Offset ([BitConverter]::GetBytes($Value))
}

function Get-U16 { param([byte[]]$Bytes, [int]$Offset)
    return [BitConverter]::ToUInt16($Bytes, $Offset)
}
function Get-U32 { param([byte[]]$Bytes, [int]$Offset)
    return [BitConverter]::ToUInt32($Bytes, $Offset)
}
function Get-U64 { param([byte[]]$Bytes, [int]$Offset)
    return [BitConverter]::ToUInt64($Bytes, $Offset)
}
function Get-I64 { param([byte[]]$Bytes, [int]$Offset)
    return [BitConverter]::ToInt64($Bytes, $Offset)
}

function Set-Ascii {
    param([byte[]]$Bytes, [int]$Offset, [string]$Value)
    $encoded = [Text.Encoding]::ASCII.GetBytes($Value)
    [Array]::Copy($encoded, 0, $Bytes, $Offset, $encoded.Length)
}

function Get-Ascii {
    param([byte[]]$Bytes, [int]$Offset, [int]$Count)
    return [Text.Encoding]::ASCII.GetString($Bytes, $Offset, $Count)
}

function Align-4096 {
    param([int64]$Value)
    $adjusted = $Value + 4095L
    return [int64](($adjusted - ($adjusted % 4096L)) / 4096L) * 4096L
}

function Assert-ZeroRange {
    param([byte[]]$Bytes, [int]$Offset, [int]$Count, [string]$Context)
    for ($index = $Offset; $index -lt $Offset + $Count; ++$index) {
        if ($Bytes[$index] -ne 0) {
            Stop-FormatValidation 'NVME_BLOCK_CORRUPT' "$Context must be zero"
        }
    }
}

function New-FrameDefinition {
    param(
        [uint64]$Sequence,
        [uint64]$CameraFrame,
        [uint64]$Ticks,
        [uint64]$DeltaNs,
        [int64]$ReceivedUtcNs,
        [bool]$Corrected,
        [bool]$Incomplete,
        [uint64]$Revision,
        [byte[]]$Payload
    )
    if ($Corrected) {
        $correctedNs = $ReceivedUtcNs - 2000000L
        $offsetNs = 250000L
        $uncertaintyNs = 500000L
        $clockSource = 1
        $syncState = 1
    }
    else {
        $correctedNs = 0L
        $offsetNs = 0L
        $uncertaintyNs = 0L
        $clockSource = 5
        $syncState = 4
    }
    return [pscustomobject][ordered]@{
        sequence = $Sequence
        cameraFrame = $CameraFrame
        ticks = $Ticks
        frequency = 1000000000L
        deltaNs = $DeltaNs
        receivedUtcNs = $ReceivedUtcNs
        corrected = $Corrected
        correctedUtcNs = $correctedNs
        offsetNs = $offsetNs
        uncertaintyNs = $uncertaintyNs
        revision = $Revision
        incomplete = $Incomplete
        payload = $Payload
        width = 2
        height = 2
        stride = 2
        pixelFormat = 1
        clockSource = $clockSource
        syncState = $syncState
    }
}

function Get-ScenarioDefinition {
    param([string]$Name)
    $baseUtc = 1786665600000000000L
    switch ($Name) {
        'minimal' {
            $frames = @(New-FrameDefinition 100 1000 5000000000 0 $baseUtc $true $false 7 ([byte[]](1, 2, 3, 4)))
        }
        'multi-frame' {
            $frames = @(
                (New-FrameDefinition 200 2000 6000000000 0 $baseUtc $true $false 7 ([byte[]](10, 11, 12, 13))),
                (New-FrameDefinition 201 2001 6016666667 16666667 ($baseUtc + 16666667L) $true $false 7 ([byte[]](20, 21, 22, 23))),
                (New-FrameDefinition 202 2002 6033333334 33333334 ($baseUtc + 33333334L) $true $false 8 ([byte[]](30, 31, 32, 33)))
            )
        }
        'incomplete-frame' {
            $frames = @(New-FrameDefinition 300 3000 7000000000 0 $baseUtc $true $true 7 ([byte[]](40, 41, 42)))
        }
        'uncorrected-time' {
            $frames = @(New-FrameDefinition 400 4000 8000000000 0 $baseUtc $false $false 0 ([byte[]](50, 51, 52, 53)))
        }
        default { throw "Unknown scenario $Name" }
    }
    return [pscustomobject]@{ name = $Name; frames = $frames }
}

function New-Pbnvme3Block {
    param([object]$Scenario)
    $frames = @($Scenario.frames)
    $capacity = $frames.Count
    $maxFrameBytes = [int](($frames | ForEach-Object { $_.payload.Length } | Measure-Object -Maximum).Maximum)
    $indexRegionBytes = [int](Align-4096 ([int64]$capacity * $script:IndexEntryBytes))
    $dataRegionBytes = [int](Align-4096 ([int64]$capacity * $maxFrameBytes))
    $dataStart = $script:HeaderBytes + $indexRegionBytes
    $footerStart = $dataStart + $dataRegionBytes
    $fileBytes = $footerStart + $script:FooterBytes
    [byte[]]$bytes = New-Object byte[] $fileBytes

    Set-Ascii $bytes 0 "PBNVME3`0"
    Set-U16 $bytes 8 3
    Set-U16 $bytes 10 $script:HeaderBytes
    Set-U32 $bytes 12 0x01020304
    [byte[]]$uuidBytes = 0x01,0x9f,0xff,0x90,0x30,0x82,0x44,0xb2,0x97,0xcd,0xc9,0xa4,0xa6,0xec,0xcd,0x3b
    [Array]::Copy($uuidBytes, 0, $bytes, 16, 16)
    Set-U64 $bytes 32 1
    Set-Ascii $bytes 40 'CAM01'
    Set-U16 $bytes 56 5
    Set-U16 $bytes 58 1
    Set-U32 $bytes 60 2
    Set-U32 $bytes 64 2
    Set-U32 $bytes 68 2
    Set-U32 $bytes 72 0
    Set-U32 $bytes 76 1000
    Set-U32 $bytes 80 $script:IndexEntryBytes
    Set-U32 $bytes 84 $capacity
    Set-U32 $bytes 88 $script:AlignmentBytes
    Set-U32 $bytes 92 $maxFrameBytes
    Set-I64 $bytes 96 $frames[0].receivedUtcNs
    Set-I64 $bytes 104 1234567890000000L
    Set-U64 $bytes 112 $frames[0].sequence
    Set-U64 $bytes 120 ([uint64]$script:RuntimeInstanceHash)
    $headerCrc = Get-Crc32C $bytes 0 $script:HeaderBytes 128 4
    Set-U32 $bytes 128 $headerCrc

    $dataCursor = $dataStart
    $containsIncomplete = $false
    for ($frameIndex = 0; $frameIndex -lt $frames.Count; ++$frameIndex) {
        $frame = $frames[$frameIndex]
        $entryOffset = $script:HeaderBytes + $frameIndex * $script:IndexEntryBytes
        Set-U64 $bytes ($entryOffset + 0) $frame.sequence
        Set-U64 $bytes ($entryOffset + 8) $frame.cameraFrame
        Set-U64 $bytes ($entryOffset + 16) $frame.ticks
        Set-U64 $bytes ($entryOffset + 24) $frame.frequency
        Set-U64 $bytes ($entryOffset + 32) $frame.deltaNs
        Set-I64 $bytes ($entryOffset + 40) $frame.receivedUtcNs
        Set-I64 $bytes ($entryOffset + 48) $frame.correctedUtcNs
        Set-I64 $bytes ($entryOffset + 56) $frame.offsetNs
        Set-I64 $bytes ($entryOffset + 64) $frame.uncertaintyNs
        Set-U64 $bytes ($entryOffset + 72) $frame.revision
        Set-U64 $bytes ($entryOffset + 80) $dataCursor
        Set-U32 $bytes ($entryOffset + 88) $frame.payload.Length
        Set-U32 $bytes ($entryOffset + 92) $frame.width
        Set-U32 $bytes ($entryOffset + 96) $frame.height
        Set-U32 $bytes ($entryOffset + 100) $frame.stride
        Set-U16 $bytes ($entryOffset + 104) $frame.pixelFormat
        $bytes[$entryOffset + 106] = [byte]$frame.clockSource
        $bytes[$entryOffset + 107] = [byte]$frame.syncState
        [uint32]$flags = 2
        if ($frame.incomplete) { $flags = $flags -bor 1; $containsIncomplete = $true }
        if ($frame.corrected) { $flags = $flags -bor 4 -bor 8 -bor 16 }
        Set-U32 $bytes ($entryOffset + 108) $flags
        [Array]::Copy($frame.payload, 0, $bytes, $dataCursor, $frame.payload.Length)
        Set-U32 $bytes ($entryOffset + 112) (Get-Crc32C $bytes $dataCursor $frame.payload.Length)
        Set-U32 $bytes ($entryOffset + 116) (Get-Crc32C $bytes $entryOffset $script:IndexEntryBytes ($entryOffset + 116) 4)
        $dataCursor += $frame.payload.Length
    }

    $validIndexBytes = $frames.Count * $script:IndexEntryBytes
    $validDataBytes = $dataCursor - $dataStart
    $indexCrc = Get-Crc32C $bytes $script:HeaderBytes $validIndexBytes
    $dataCrc = Get-Crc32C $bytes $dataStart $validDataBytes
    Set-Ascii $bytes $footerStart 'PBCOMMIT'
    Set-U16 $bytes ($footerStart + 8) 3
    Set-U16 $bytes ($footerStart + 10) $script:FooterBytes
    Set-U32 $bytes ($footerStart + 12) $frames.Count
    Set-U64 $bytes ($footerStart + 16) $validIndexBytes
    Set-U64 $bytes ($footerStart + 24) $validDataBytes
    Set-U64 $bytes ($footerStart + 32) $fileBytes
    Set-I64 $bytes ($footerStart + 40) $frames[-1].receivedUtcNs
    Set-U64 $bytes ($footerStart + 48) $frames[-1].sequence
    Set-U32 $bytes ($footerStart + 56) $indexCrc
    Set-U32 $bytes ($footerStart + 60) $dataCrc
    Set-U32 $bytes ($footerStart + 64) $headerCrc
    Set-U32 $bytes ($footerStart + 68) ([uint32]([int]$containsIncomplete))
    Set-Ascii $bytes ($footerStart + 4088) "COMMIT3`0"
    $footerCrc = Get-Crc32C $bytes $footerStart $script:FooterBytes ($footerStart + 4084) 4
    Set-U32 $bytes ($footerStart + 4084) $footerCrc

    return [pscustomobject]@{
        bytes = $bytes
        frames = $frames
        headerCrc = $headerCrc
        indexCrc = $indexCrc
        dataCrc = $dataCrc
        footerCrc = $footerCrc
        containsIncomplete = $containsIncomplete
    }
}

function New-AvailabilityValue {
    param([bool]$Available, [object]$Value, [object]$ErrorCode = $null)
    return [ordered]@{ available = $Available; value = $(if ($Available) { $Value } else { $null }); errorCode = $ErrorCode }
}

function New-ManifestV4 {
    param([object]$Scenario, [object]$Block)
    $frames = @($Block.frames)
    $corrected = [bool]$frames[0].corrected
    $syncState = if ($corrected) { 'SYNCED' } else { 'UNSYNCED' }
    $reasonCodes = @()
    if (-not $corrected) { $reasonCodes += 'TIME_CORRECTION_UNAVAILABLE' }
    if ($Block.containsIncomplete) { $reasonCodes += 'FRAME_INCOMPLETE' }
    $complete = $corrected -and -not $Block.containsIncomplete
    $overallSyncState = if ($Block.containsIncomplete) { 'DEGRADED' } else { $syncState }
    $relativePath = 'raw/CAM01/block.pbnvme3'
    $sha256 = Get-Sha256Text $Block.bytes
    $correctedStart = if ($corrected) { [string]$frames[0].correctedUtcNs } else { $null }
    $correctedEnd = if ($corrected) { [string]$frames[-1].correctedUtcNs } else { $null }
    $revisions = @($frames | Where-Object { $_.revision -gt 0 } | ForEach-Object { [uint64]$_.revision } | Select-Object -Unique)
    $clockModels = @()
    foreach ($revision in $revisions) {
        $clockModels += [ordered]@{
            machineId = 'EDGE-01'
            timeRuntimeInstanceId = $script:RuntimeInstanceId
            cameraId = 'CAM01'
            modelRevision = $revision
            clockSource = 'PTP_HARDWARE'
            syncState = 'SYNCED'
            uncertaintyAvailable = $true
            uncertaintyNs = '500000'
        }
    }
    $parameters = [ordered]@{
        exposureTimeUs = New-AvailabilityValue $true 500.0
        gainDb = New-AvailabilityValue $true 3.0
        actualFps = New-AvailabilityValue $true 60.0
        roi = New-AvailabilityValue $true ([ordered]@{ offsetX = 0; offsetY = 0; width = 2; height = 2 })
        width = New-AvailabilityValue $true 2
        height = New-AvailabilityValue $true 2
        pixelFormat = New-AvailabilityValue $true 'Mono8'
        triggerMode = New-AvailabilityValue $true 'Off'
        triggerSource = New-AvailabilityValue $false $null 'SYS_NOT_SUPPORTED'
        packetSizeBytes = New-AvailabilityValue $true 1500
        transmissionDelayNs = New-AvailabilityValue $true '0'
        timeSyncCapability = New-AvailabilityValue $true 'PTP_HARDWARE'
        timeSyncState = New-AvailabilityValue $true $syncState
    }
    $manifest = [ordered]@{
        schemaVersion = 4
        eventId = "EVT-R0-03-$($Scenario.name)"
        decisionState = 'Confirmed'
        triggerCount = 1
        candidateTime = '2026-08-14T00:00:00.000Z'
        confirmedTime = '2026-08-14T00:00:00.100Z'
        startTime = '2026-08-13T23:59:59.000Z'
        endTime = '2026-08-14T00:00:01.000Z'
        cameraIds = @('CAM01')
        triggerCameraId = 'CAM01'
        triggerFrameNumber = [uint64]$frames[0].cameraFrame
        triggerReason = 'R0-03 golden vector'
        confidence = 0.95
        preEventSeconds = 1.0
        postEventSeconds = 1.0
        algorithmName = 'contract-golden'
        algorithmVersion = '1.0.0'
        configVersion = 'schema-8-revision-42'
        machineId = 'EDGE-01'
        productionLineId = 'LINE-01'
        paperType = 'test'
        paperSpeed = $null
        uploadState = 'Pending'
        timeQuality = $(if ($complete) { 'Synchronized' } else { 'Degraded' })
        writeMode = 'buffered'
        powerLossDurable = $false
        verification = 'upload-or-on-demand'
        destinationRelativePath = "2026/08/14/EVT-R0-03-$($Scenario.name)"
        windowComplete = $complete
        truncatedByMaximumDuration = $false
        stoppedEarly = $false
        eventT0 = [ordered]@{
            available = $corrected
            timestampUtcNs = $(if ($corrected) { [string]$frames[0].correctedUtcNs } else { $null })
            triggerSource = 'ALGORITHM'
            triggerMachineId = 'EDGE-01'
            triggerCameraId = 'CAM01'
            triggerNodeId = 'EDGE-01/CAM01'
        }
        cameraActualRanges = @([ordered]@{
            machineId = 'EDGE-01'
            cameraId = 'CAM01'
            receivedStartUtcNs = [string]$frames[0].receivedUtcNs
            receivedEndUtcNs = [string]$frames[-1].receivedUtcNs
            correctedStartUtcNsAvailable = $corrected
            correctedStartUtcNs = $correctedStart
            correctedEndUtcNsAvailable = $corrected
            correctedEndUtcNs = $correctedEnd
            frameCount = $frames.Count
            sequenceGaps = 0
            complete = $complete
            syncState = $syncState
            uncertaintyAvailable = $corrected
            maximumUncertaintyNs = $(if ($corrected) { '500000' } else { $null })
            timeRuntimeInstanceId = $script:RuntimeInstanceId
            clockModelRevisions = $revisions
        })
        clockModels = $clockModels
        overallTimeQuality = [ordered]@{
            syncState = $overallSyncState
            maximumUncertaintyAvailable = $corrected
            maximumUncertaintyNs = $(if ($corrected) { '500000' } else { $null })
            reasonCodes = $reasonCodes
        }
        rawBlocks = @([ordered]@{
            path = $relativePath
            cameraId = 'CAM01'
            format = 'PBNVME3'
            formatVersion = 3
            frameCount = $frames.Count
            firstSequenceNumber = [uint64]$frames[0].sequence
            lastSequenceNumber = [uint64]$frames[-1].sequence
            receivedStartUtcNs = [string]$frames[0].receivedUtcNs
            receivedEndUtcNs = [string]$frames[-1].receivedUtcNs
            correctedStartUtcNsAvailable = $corrected
            correctedStartUtcNs = $correctedStart
            correctedEndUtcNsAvailable = $corrected
            correctedEndUtcNs = $correctedEnd
            containsIncompleteFrame = [bool]$Block.containsIncomplete
            sizeBytes = $Block.bytes.Length
            sha256 = $sha256
            headerCrc32c = [uint32]$Block.headerCrc
            indexCrc32c = [uint32]$Block.indexCrc
            dataCrc32c = [uint32]$Block.dataCrc
            footerCrc32c = [uint32]$Block.footerCrc
        })
        keyFrames = @()
        cameraConfigSnapshots = @([ordered]@{
            cameraId = 'CAM01'
            serialNumber = 'R003GOLDEN0001'
            model = 'MV-CS020-60GM'
            capturedAtUtcNs = [string]$frames[0].receivedUtcNs
            configSchemaVersion = 8
            configRevision = 42
            parameters = $parameters
        })
        fileChecksums = [ordered]@{ $relativePath = $sha256 }
        fileSizes = [ordered]@{ $relativePath = $Block.bytes.Length }
    }
    return $manifest
}

function Write-Utf8WithoutBom {
    param([string]$Path, [string]$Text)
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}

function Write-GoldenFiles {
    [IO.Directory]::CreateDirectory($goldenRoot) | Out-Null
    foreach ($name in @('minimal', 'multi-frame', 'incomplete-frame', 'uncorrected-time')) {
        $scenario = Get-ScenarioDefinition $name
        $block = New-Pbnvme3Block $scenario
        $manifest = New-ManifestV4 $scenario $block
        $directory = Join-Path $goldenRoot $name
        [IO.Directory]::CreateDirectory($directory) | Out-Null
        [IO.File]::WriteAllBytes((Join-Path $directory 'block.pbnvme3'), $block.bytes)
        $json = ($manifest | ConvertTo-Json -Depth 20) + "`n"
        Write-Utf8WithoutBom (Join-Path $directory 'manifest-v4.json') $json
    }
}

function Test-CanonicalNanoseconds {
    param([object]$Value, [bool]$Nullable, [bool]$NonNegative, [string]$Context)
    if ($null -eq $Value) {
        Assert-Format $Nullable 'EVENT_SCHEMA_INVALID' "$Context cannot be null"
        return
    }
    Assert-Format ($Value -is [string]) 'EVENT_SCHEMA_INVALID' "$Context must be a decimal string"
    Assert-Format ($Value -cmatch '^(0|-?[1-9][0-9]{0,18})$') 'EVENT_SCHEMA_INVALID' "$Context is not canonical"
    [int64]$parsed = 0
    $valid = [int64]::TryParse($Value, [Globalization.NumberStyles]::AllowLeadingSign,
        [Globalization.CultureInfo]::InvariantCulture, [ref]$parsed)
    Assert-Format $valid 'EVENT_SCHEMA_INVALID' "$Context exceeds int64"
    Assert-Format (-not $NonNegative -or $parsed -ge 0) 'EVENT_SCHEMA_INVALID' "$Context must be non-negative"
}

function Test-AvailabilityPair {
    param([object]$Object, [string]$Flag, [string]$Value, [string]$Context, [bool]$NonNegative = $false)
    Assert-Format ($Object.$Flag -is [bool]) 'EVENT_SCHEMA_INVALID' "$Context.$Flag must be bool"
    Assert-Format ($Object.$Flag -eq ($null -ne $Object.$Value)) 'EVENT_SCHEMA_INVALID' "$Context availability is inconsistent"
    Test-CanonicalNanoseconds $Object.$Value $true $NonNegative "$Context.$Value"
}

function Test-Pbnvme3Block {
    param([byte[]]$Bytes, [string]$Context)
    Assert-Format ($Bytes.Length -ge $script:HeaderBytes) 'NVME_BLOCK_INCOMPLETE' "$Context lacks header"
    Assert-Format ((Get-Ascii $Bytes 0 8) -ceq "PBNVME3`0") 'NVME_FORMAT_UNSUPPORTED' "$Context magic is unsupported"
    $version = Get-U16 $Bytes 8
    Assert-Format ($version -eq 3) 'NVME_FORMAT_UNSUPPORTED' "$Context version $version is unsupported"
    Assert-Format ((Get-U16 $Bytes 10) -eq 4096 -and (Get-U32 $Bytes 12) -eq 0x01020304) 'NVME_BLOCK_CORRUPT' "$Context header constants are invalid"
    Assert-Format ((Get-U32 $Bytes 72) -eq 0 -and (Get-U32 $Bytes 76) -eq 1000 -and
        (Get-U32 $Bytes 80) -eq 160 -and (Get-U32 $Bytes 88) -eq 4096) 'NVME_BLOCK_CORRUPT' "$Context header policy is invalid"
    $cameraIdBytes = Get-U16 $Bytes 56
    Assert-Format ($cameraIdBytes -ge 1 -and $cameraIdBytes -le 16) 'NVME_BLOCK_CORRUPT' "$Context cameraId length is invalid"
    $cameraId = [Text.Encoding]::UTF8.GetString($Bytes, 40, $cameraIdBytes)
    Assert-Format ($cameraId -cmatch '^CAM0[1-6]$') 'NVME_BLOCK_CORRUPT' "$Context cameraId is invalid"
    Assert-ZeroRange $Bytes (40 + $cameraIdBytes) (16 - $cameraIdBytes) "$Context cameraId padding"
    $initialPixelFormat = Get-U16 $Bytes 58
    Assert-Format ($script:KnownPixelFormats -contains [int]$initialPixelFormat) 'NVME_BLOCK_CORRUPT' "$Context pixel format is invalid"
    Assert-Format ((Get-U32 $Bytes 60) -gt 0 -and (Get-U32 $Bytes 64) -gt 0 -and (Get-U32 $Bytes 68) -gt 0) 'NVME_BLOCK_CORRUPT' "$Context initial geometry is invalid"
    $capacity = [int](Get-U32 $Bytes 84)
    $maxFrameBytes = [int64](Get-U32 $Bytes 92)
    Assert-Format ($capacity -ge 1 -and $capacity -le $script:MaximumIndexCapacity) 'NVME_BLOCK_CORRUPT' "$Context index capacity is invalid"
    Assert-Format ($maxFrameBytes -ge 1 -and $maxFrameBytes -le $script:MaximumFrameBytes) 'NVME_BLOCK_CORRUPT' "$Context maximum frame size is invalid"
    $indexRegionBytes = Align-4096 ([int64]$capacity * $script:IndexEntryBytes)
    $dataRegionBytes = Align-4096 ([int64]$capacity * $maxFrameBytes)
    $expectedFileBytes = [int64]$script:HeaderBytes + $indexRegionBytes + $dataRegionBytes + $script:FooterBytes
    Assert-Format ($expectedFileBytes -le $script:MaximumBlockBytes) 'NVME_BLOCK_CORRUPT' "$Context exceeds maximum block bytes"
    Assert-Format ($Bytes.LongLength -ge $expectedFileBytes) 'NVME_BLOCK_INCOMPLETE' "$Context is missing the fixed footer"
    Assert-Format ($Bytes.LongLength -eq $expectedFileBytes) 'NVME_BLOCK_CORRUPT' "$Context file length is not exact"
    $storedHeaderCrc = Get-U32 $Bytes 128
    Assert-Format ($storedHeaderCrc -eq (Get-Crc32C $Bytes 0 4096 128 4)) 'NVME_BLOCK_CORRUPT' "$Context header CRC differs"
    Assert-ZeroRange $Bytes 132 3964 "$Context header reserved"

    $footerStart = [int]($expectedFileBytes - $script:FooterBytes)
    Assert-Format ((Get-Ascii $Bytes $footerStart 8) -ceq 'PBCOMMIT' -and
        (Get-Ascii $Bytes ($footerStart + 4088) 8) -ceq "COMMIT3`0") 'NVME_BLOCK_INCOMPLETE' "$Context commit marker is absent"
    Assert-Format ((Get-U16 $Bytes ($footerStart + 8)) -eq 3 -and
        (Get-U16 $Bytes ($footerStart + 10)) -eq 4096) 'NVME_BLOCK_CORRUPT' "$Context footer version is invalid"
    $storedFooterCrc = Get-U32 $Bytes ($footerStart + 4084)
    Assert-Format ($storedFooterCrc -eq (Get-Crc32C $Bytes $footerStart 4096 ($footerStart + 4084) 4)) 'NVME_BLOCK_CORRUPT' "$Context footer CRC differs"
    $frameCount = [int](Get-U32 $Bytes ($footerStart + 12))
    Assert-Format ($frameCount -ge 1 -and $frameCount -le $capacity) 'NVME_BLOCK_CORRUPT' "$Context frame count is invalid"
    Assert-Format ((Get-U64 $Bytes ($footerStart + 16)) -eq [uint64]($frameCount * 160) -and
        (Get-U64 $Bytes ($footerStart + 32)) -eq [uint64]$expectedFileBytes -and
        (Get-U32 $Bytes ($footerStart + 64)) -eq $storedHeaderCrc) 'NVME_BLOCK_CORRUPT' "$Context footer echoes are invalid"
    $footerFlags = Get-U32 $Bytes ($footerStart + 68)
    Assert-Format (($footerFlags -band 0xfffffffe) -eq 0) 'NVME_BLOCK_CORRUPT' "$Context footer has unknown flags"
    Assert-ZeroRange $Bytes ($footerStart + 72) 4012 "$Context footer reserved"

    $dataStart = [int]($script:HeaderBytes + $indexRegionBytes)
    $dataCursor = $dataStart
    $previousSequence = [uint64]0
    $previousDelta = [uint64]0
    $containsIncomplete = $false
    $correctedAll = $true
    $firstReceived = 0L
    $lastReceived = 0L
    $firstCorrected = $null
    $lastCorrected = $null
    $revisions = @()
    for ($frameIndex = 0; $frameIndex -lt $frameCount; ++$frameIndex) {
        $entryOffset = $script:HeaderBytes + $frameIndex * $script:IndexEntryBytes
        $storedEntryCrc = Get-U32 $Bytes ($entryOffset + 116)
        Assert-Format ($storedEntryCrc -eq (Get-Crc32C $Bytes $entryOffset 160 ($entryOffset + 116) 4)) 'NVME_BLOCK_CORRUPT' "$Context entry $frameIndex CRC differs"
        Assert-ZeroRange $Bytes ($entryOffset + 120) 40 "$Context entry $frameIndex reserved"
        $sequence = Get-U64 $Bytes $entryOffset
        $delta = Get-U64 $Bytes ($entryOffset + 32)
        Assert-Format (($frameIndex -eq 0 -or $sequence -gt $previousSequence) -and
            ($frameIndex -eq 0 -or $delta -ge $previousDelta) -and
            ($frameIndex -ne 0 -or $delta -eq 0)) 'NVME_BLOCK_CORRUPT' "$Context frame ordering is invalid"
        $previousSequence = $sequence
        $previousDelta = $delta
        $flags = Get-U32 $Bytes ($entryOffset + 108)
        Assert-Format (($flags -band 0xffffffe0) -eq 0) 'NVME_BLOCK_CORRUPT' "$Context entry $frameIndex has unknown flags"
        $incomplete = ($flags -band 1) -ne 0
        $ticksAvailable = ($flags -band 2) -ne 0
        $correctedAvailable = ($flags -band 4) -ne 0
        $offsetAvailable = ($flags -band 8) -ne 0
        $uncertaintyAvailable = ($flags -band 16) -ne 0
        $containsIncomplete = $containsIncomplete -or $incomplete
        $ticks = Get-U64 $Bytes ($entryOffset + 16)
        $frequency = Get-U64 $Bytes ($entryOffset + 24)
        Assert-Format (($ticksAvailable -and $frequency -gt 0) -or
            (-not $ticksAvailable -and $ticks -eq 0 -and $frequency -eq 0)) 'NVME_BLOCK_CORRUPT' "$Context ticks availability is invalid"
        $correctedUtc = Get-I64 $Bytes ($entryOffset + 48)
        $offsetNs = Get-I64 $Bytes ($entryOffset + 56)
        $uncertaintyNs = Get-I64 $Bytes ($entryOffset + 64)
        $revision = Get-U64 $Bytes ($entryOffset + 72)
        $clockSource = [int]$Bytes[$entryOffset + 106]
        $syncState = [int]$Bytes[$entryOffset + 107]
        Assert-Format ($script:KnownClockSources -contains $clockSource -and $script:KnownSyncStates -contains $syncState) 'NVME_BLOCK_CORRUPT' "$Context time enum is invalid"
        Assert-Format ($offsetAvailable -or $offsetNs -eq 0) 'NVME_BLOCK_CORRUPT' "$Context unavailable offset is nonzero"
        Assert-Format (($uncertaintyAvailable -and $uncertaintyNs -ge 0) -or
            (-not $uncertaintyAvailable -and $uncertaintyNs -eq 0)) 'NVME_BLOCK_CORRUPT' "$Context uncertainty is invalid"
        Assert-Format (($correctedAvailable -and $uncertaintyAvailable -and $revision -gt 0 -and
            $syncState -notin @(0, 4)) -or (-not $correctedAvailable -and $correctedUtc -eq 0 -and
            $syncState -in @(0, 4))) 'NVME_BLOCK_CORRUPT' "$Context corrected time is inconsistent"
        $payloadOffset = Get-U64 $Bytes ($entryOffset + 80)
        $payloadBytes = [int64](Get-U32 $Bytes ($entryOffset + 88))
        $width = Get-U32 $Bytes ($entryOffset + 92)
        $height = Get-U32 $Bytes ($entryOffset + 96)
        $stride = Get-U32 $Bytes ($entryOffset + 100)
        $pixelFormat = Get-U16 $Bytes ($entryOffset + 104)
        Assert-Format ($payloadOffset -eq [uint64]$dataCursor -and $payloadBytes -ge 1 -and
            $payloadBytes -le $maxFrameBytes -and $payloadOffset + [uint64]$payloadBytes -le [uint64]$footerStart) 'NVME_BLOCK_CORRUPT' "$Context payload range is invalid"
        Assert-Format ($width -gt 0 -and $height -gt 0 -and $stride -gt 0 -and
            [uint64]$payloadBytes -le [uint64]$stride * [uint64]$height -and
            $script:KnownPixelFormats -contains [int]$pixelFormat) 'NVME_BLOCK_CORRUPT' "$Context geometry is invalid"
        Assert-Format ((Get-U32 $Bytes ($entryOffset + 112)) -eq
            (Get-Crc32C $Bytes ([int]$payloadOffset) ([int]$payloadBytes))) 'NVME_BLOCK_CORRUPT' "$Context payload CRC differs"
        $received = Get-I64 $Bytes ($entryOffset + 40)
        if ($frameIndex -eq 0) { $firstReceived = $received; if ($correctedAvailable) { $firstCorrected = $correctedUtc } }
        $lastReceived = $received
        if ($correctedAvailable) { $lastCorrected = $correctedUtc; $revisions += [uint64]$revision } else { $correctedAll = $false }
        $dataCursor += [int]$payloadBytes
    }
    Assert-ZeroRange $Bytes ($script:HeaderBytes + $frameCount * 160) ([int]$indexRegionBytes - $frameCount * 160) "$Context unused index region"
    $validDataBytes = $dataCursor - $dataStart
    Assert-ZeroRange $Bytes $dataCursor ($footerStart - $dataCursor) "$Context data padding"
    Assert-Format ((Get-U64 $Bytes ($footerStart + 24)) -eq [uint64]$validDataBytes -and
        (Get-I64 $Bytes ($footerStart + 40)) -eq $lastReceived -and
        (Get-U64 $Bytes ($footerStart + 48)) -eq $previousSequence) 'NVME_BLOCK_CORRUPT' "$Context footer ranges differ"
    Assert-Format ((Get-U32 $Bytes ($footerStart + 56)) -eq
        (Get-Crc32C $Bytes $script:HeaderBytes ($frameCount * 160)) -and
        (Get-U32 $Bytes ($footerStart + 60)) -eq
        (Get-Crc32C $Bytes $dataStart $validDataBytes)) 'NVME_BLOCK_CORRUPT' "$Context region CRC differs"
    Assert-Format ((($footerFlags -band 1) -ne 0) -eq $containsIncomplete) 'NVME_BLOCK_CORRUPT' "$Context incomplete aggregate differs"
    return [pscustomobject]@{
        cameraId = $cameraId
        frameCount = $frameCount
        firstSequence = Get-U64 $Bytes $script:HeaderBytes
        lastSequence = $previousSequence
        firstReceivedUtcNs = $firstReceived
        lastReceivedUtcNs = $lastReceived
        correctedAll = $correctedAll
        firstCorrectedUtcNs = $firstCorrected
        lastCorrectedUtcNs = $lastCorrected
        containsIncomplete = $containsIncomplete
        headerCrc = $storedHeaderCrc
        indexCrc = Get-U32 $Bytes ($footerStart + 56)
        dataCrc = Get-U32 $Bytes ($footerStart + 60)
        footerCrc = $storedFooterCrc
        revisions = @($revisions | Select-Object -Unique)
    }
}

function Get-PropertyNames { param([object]$Value)
    return @($Value.PSObject.Properties.Name)
}

function Assert-ObjectFields {
    param([object]$Value, [string[]]$Required, [string[]]$Optional, [string]$Context)
    Assert-Format ($null -ne $Value) 'EVENT_SCHEMA_INVALID' "$Context must be an object"
    $actual = Get-PropertyNames $Value
    foreach ($field in $Required) {
        Assert-Format ($actual -ccontains $field) 'EVENT_SCHEMA_INVALID' "$Context lacks $field"
    }
    foreach ($field in $actual) {
        Assert-Format (($Required -ccontains $field) -or ($Optional -ccontains $field)) 'EVENT_SCHEMA_INVALID' "$Context has unknown field $field"
    }
}

function Test-ManifestV4 {
    param([byte[]]$JsonBytes, [byte[]]$BlockBytes, [object]$BlockSummary, [string]$Context)
    Assert-Format ($JsonBytes.Length -le $script:MaximumManifestBytes) 'EVENT_SCHEMA_INVALID' "$Context exceeds manifest limit"
    Assert-Format (-not ($JsonBytes.Length -ge 3 -and $JsonBytes[0] -eq 0xef -and $JsonBytes[1] -eq 0xbb -and $JsonBytes[2] -eq 0xbf)) 'EVENT_SCHEMA_INVALID' "$Context contains a BOM"
    try { $manifest = [Text.Encoding]::UTF8.GetString($JsonBytes) | ConvertFrom-Json }
    catch { Stop-FormatValidation 'EVENT_SCHEMA_INVALID' "$Context is not JSON" }
    Assert-Format ((Get-PropertyNames $manifest) -ccontains 'schemaVersion') 'EVENT_SCHEMA_INVALID' "$Context lacks schemaVersion"
    Assert-Format ($manifest.schemaVersion -eq 4) 'EVENT_SCHEMA_UNSUPPORTED' "$Context schema is unsupported"
    $contract = [IO.File]::ReadAllText($manifestContractPath, [Text.Encoding]::UTF8) | ConvertFrom-Json
    $required = @($contract.requiredRootFields)
    $optional = @('triggerCount', 'triggerFrameNumber', 'confidence', 'preEventSeconds',
        'postEventSeconds', 'paperType', 'paperSpeed', 'timeQuality', 'windowComplete',
        'truncatedByMaximumDuration', 'stoppedEarly', 'extensions')
    Assert-ObjectFields $manifest $required $optional $Context
    Assert-Format ($manifest.writeMode -ceq 'buffered' -and $manifest.powerLossDurable -is [bool] -and
        -not $manifest.powerLossDurable -and $manifest.verification -ceq 'upload-or-on-demand') 'EVENT_SCHEMA_INVALID' "$Context write policy differs"
    Assert-Format (@($manifest.cameraIds).Count -ge 1 -and @($manifest.cameraIds).Count -le 6 -and
        @($manifest.cameraIds) -contains $manifest.triggerCameraId) 'EVENT_SCHEMA_INVALID' "$Context camera list is invalid"
    Assert-ObjectFields $manifest.eventT0 @('available','timestampUtcNs','triggerSource','triggerMachineId','triggerCameraId','triggerNodeId') @() "$Context.eventT0"
    Assert-Format ($manifest.eventT0.available -is [bool] -and
        $manifest.eventT0.available -eq ($null -ne $manifest.eventT0.timestampUtcNs)) 'EVENT_SCHEMA_INVALID' "$Context T0 availability differs"
    Test-CanonicalNanoseconds $manifest.eventT0.timestampUtcNs $true $false "$Context.eventT0.timestampUtcNs"
    Assert-Format (@('ALGORITHM','MANUAL','PLANT_IO','EXTERNAL') -ccontains $manifest.eventT0.triggerSource) 'EVENT_SCHEMA_INVALID' "$Context trigger source is invalid"
    Assert-Format ($manifest.eventT0.triggerMachineId -ceq $manifest.machineId -and
        $manifest.eventT0.triggerCameraId -ceq $manifest.triggerCameraId) 'EVENT_SCHEMA_INVALID' "$Context T0 identity differs"

    $ranges = @($manifest.cameraActualRanges)
    Assert-Format ($ranges.Count -eq @($manifest.cameraIds).Count) 'EVENT_SCHEMA_INVALID' "$Context actual ranges differ from cameras"
    $range = $ranges[0]
    Assert-ObjectFields $range @('machineId','cameraId','receivedStartUtcNs','receivedEndUtcNs',
        'correctedStartUtcNsAvailable','correctedStartUtcNs','correctedEndUtcNsAvailable',
        'correctedEndUtcNs','frameCount','sequenceGaps','complete','syncState',
        'uncertaintyAvailable','maximumUncertaintyNs','timeRuntimeInstanceId','clockModelRevisions') @() "$Context.cameraActualRange"
    Test-CanonicalNanoseconds $range.receivedStartUtcNs $false $false "$Context.receivedStartUtcNs"
    Test-CanonicalNanoseconds $range.receivedEndUtcNs $false $false "$Context.receivedEndUtcNs"
    Test-AvailabilityPair $range 'correctedStartUtcNsAvailable' 'correctedStartUtcNs' "$Context.range.start"
    Test-AvailabilityPair $range 'correctedEndUtcNsAvailable' 'correctedEndUtcNs' "$Context.range.end"
    Test-AvailabilityPair $range 'uncertaintyAvailable' 'maximumUncertaintyNs' "$Context.range.uncertainty" $true
    Assert-Format ($range.cameraId -ceq $BlockSummary.cameraId -and $range.frameCount -eq $BlockSummary.frameCount -and
        [string]$range.receivedStartUtcNs -ceq [string]$BlockSummary.firstReceivedUtcNs -and
        [string]$range.receivedEndUtcNs -ceq [string]$BlockSummary.lastReceivedUtcNs) 'EVENT_SCHEMA_INVALID' "$Context actual range differs from block"
    Assert-Format ($range.correctedStartUtcNsAvailable -eq $BlockSummary.correctedAll) 'EVENT_SCHEMA_INVALID' "$Context corrected range differs"

    Assert-Format (@($manifest.clockModels).Count -le 64) 'EVENT_SCHEMA_INVALID' "$Context has too many models"
    foreach ($model in @($manifest.clockModels)) {
        Assert-ObjectFields $model @('machineId','timeRuntimeInstanceId','cameraId','modelRevision','clockSource','syncState','uncertaintyAvailable','uncertaintyNs') @() "$Context.clockModel"
        Test-AvailabilityPair $model 'uncertaintyAvailable' 'uncertaintyNs' "$Context.clockModel.uncertainty" $true
        Assert-Format ($model.modelRevision -gt 0 -and @('PTP_HARDWARE','PTP_SOFTWARE','NTP','OFFSET_MODEL','RECEIVE_CLOCK','UNKNOWN') -ccontains $model.clockSource -and
            @('SYNCED','SYNCING','DEGRADED','UNSYNCED','UNKNOWN') -ccontains $model.syncState) 'EVENT_SCHEMA_INVALID' "$Context clock model is invalid"
    }
    $modelRevisions = @($manifest.clockModels | ForEach-Object { [uint64]$_.modelRevision })
    foreach ($revision in @($BlockSummary.revisions)) {
        Assert-Format ($modelRevisions -contains [uint64]$revision) 'EVENT_SCHEMA_INVALID' "$Context lacks block model revision"
    }

    $quality = $manifest.overallTimeQuality
    Assert-ObjectFields $quality @('syncState','maximumUncertaintyAvailable','maximumUncertaintyNs','reasonCodes') @() "$Context.overallTimeQuality"
    Test-AvailabilityPair $quality 'maximumUncertaintyAvailable' 'maximumUncertaintyNs' "$Context.quality.uncertainty" $true
    Assert-Format (@('SYNCED','SYNCING','DEGRADED','UNSYNCED','UNKNOWN') -ccontains $quality.syncState) 'EVENT_SCHEMA_INVALID' "$Context quality state is invalid"
    if ($BlockSummary.containsIncomplete -or -not $BlockSummary.correctedAll) {
        Assert-Format (-not $range.complete -and $quality.syncState -cne 'SYNCED') 'EVENT_SCHEMA_INVALID' "$Context degraded evidence claims complete sync"
    }

    $rawBlocks = @($manifest.rawBlocks)
    Assert-Format ($rawBlocks.Count -eq 1) 'EVENT_SCHEMA_INVALID' "$Context golden manifest must contain one block"
    $raw = $rawBlocks[0]
    Assert-ObjectFields $raw @('path','cameraId','format','formatVersion','frameCount','firstSequenceNumber',
        'lastSequenceNumber','receivedStartUtcNs','receivedEndUtcNs','correctedStartUtcNsAvailable',
        'correctedStartUtcNs','correctedEndUtcNsAvailable','correctedEndUtcNs','containsIncompleteFrame',
        'sizeBytes','sha256','headerCrc32c','indexCrc32c','dataCrc32c','footerCrc32c') @() "$Context.rawBlock"
    Test-CanonicalNanoseconds $raw.receivedStartUtcNs $false $false "$Context.raw.receivedStartUtcNs"
    Test-CanonicalNanoseconds $raw.receivedEndUtcNs $false $false "$Context.raw.receivedEndUtcNs"
    Test-AvailabilityPair $raw 'correctedStartUtcNsAvailable' 'correctedStartUtcNs' "$Context.raw.start"
    Test-AvailabilityPair $raw 'correctedEndUtcNsAvailable' 'correctedEndUtcNs' "$Context.raw.end"
    $sha = Get-Sha256Text $BlockBytes
    Assert-Format ($raw.format -ceq 'PBNVME3' -and $raw.formatVersion -eq 3 -and
        $raw.cameraId -ceq $BlockSummary.cameraId -and $raw.frameCount -eq $BlockSummary.frameCount -and
        $raw.firstSequenceNumber -eq $BlockSummary.firstSequence -and $raw.lastSequenceNumber -eq $BlockSummary.lastSequence -and
        $raw.sizeBytes -eq $BlockBytes.Length -and $raw.sha256 -ceq $sha -and
        $raw.headerCrc32c -eq $BlockSummary.headerCrc -and $raw.indexCrc32c -eq $BlockSummary.indexCrc -and
        $raw.dataCrc32c -eq $BlockSummary.dataCrc -and $raw.footerCrc32c -eq $BlockSummary.footerCrc -and
        $raw.containsIncompleteFrame -eq $BlockSummary.containsIncomplete) 'EVENT_CHECKSUM_FAILED' "$Context raw block declaration differs"
    Assert-Format (@($manifest.fileChecksums.PSObject.Properties).Count -le 262208 -and
        $manifest.fileChecksums.($raw.path) -ceq $sha -and $manifest.fileSizes.($raw.path) -eq $BlockBytes.Length) 'EVENT_CHECKSUM_FAILED' "$Context file tables differ"

    Assert-Format (@($manifest.cameraConfigSnapshots).Count -eq @($manifest.cameraIds).Count) 'EVENT_SCHEMA_INVALID' "$Context config snapshots differ"
    $minimumParameters = @($contract.objects.cameraConfigSnapshot.minimumParameters)
    foreach ($snapshot in @($manifest.cameraConfigSnapshots)) {
        Assert-ObjectFields $snapshot @('cameraId','serialNumber','model','capturedAtUtcNs','configSchemaVersion','configRevision','parameters') @() "$Context.configSnapshot"
        Test-CanonicalNanoseconds $snapshot.capturedAtUtcNs $false $false "$Context.configSnapshot.capturedAtUtcNs"
        foreach ($name in $minimumParameters) {
            Assert-Format ((Get-PropertyNames $snapshot.parameters) -ccontains $name) 'EVENT_SCHEMA_INVALID' "$Context config snapshot lacks $name"
            $parameter = $snapshot.parameters.$name
            Assert-ObjectFields $parameter @('available','value','errorCode') @() "$Context.parameters.$name"
            Assert-Format ($parameter.available -is [bool] -and
                ($parameter.available -or $null -eq $parameter.value)) 'EVENT_SCHEMA_INVALID' "$Context parameter $name availability differs"
        }
    }
    return $manifest
}

function Expect-FormatFailure {
    param([scriptblock]$Action, [string]$ExpectedCode, [string]$Context)
    try {
        & $Action
        Stop-FormatValidation 'R003_VALIDATOR_FAILED' "$Context was unexpectedly accepted"
    }
    catch {
        if ($_.Exception.Message -like 'R003_VALIDATOR_FAILED|*') { throw }
        $actual = ($_.Exception.Message -split '\|', 2)[0]
        Assert-Format ($actual -ceq $ExpectedCode) 'R003_VALIDATOR_FAILED' "$Context returned $actual instead of $ExpectedCode"
    }
}

function Copy-Bytes { param([byte[]]$Bytes)
    [byte[]]$copy = New-Object byte[] $Bytes.Length
    [Array]::Copy($Bytes, $copy, $Bytes.Length)
    return ,$copy
}

function Test-LegacyContracts {
    $v2Path = Join-Path $repositoryRoot 'docs\validation\m7-01\nvme-block-format-v2.json'
    $v2BytesBefore = [IO.File]::ReadAllBytes($v2Path)
    $v2 = [Text.Encoding]::UTF8.GetString($v2BytesBefore) | ConvertFrom-Json
    Assert-Format ($v2.schemaVersion -eq 2 -and $v2.format.formatVersion -eq 2 -and
        $v2.format.headerMagicAsciiEscaped -ceq 'PBNVME2\0' -and
        $v2.format.commitMarkerAsciiEscaped -ceq 'COMMIT2\0' -and
        $v2.writePolicy.mode -ceq 'buffered' -and -not $v2.writePolicy.flushFileBuffers -and
        $v2.writePolicy.publish -ceq 'close-then-same-volume-atomic-rename-no-overwrite') 'R003_LEGACY_REGRESSION' 'PBNVME2 contract changed'
    $eventHeaderPath = Join-Path $repositoryRoot 'src\storage\include\paperbreak\storage\event_store.hpp'
    $eventHeaderBefore = [IO.File]::ReadAllBytes($eventHeaderPath)
    $eventHeaderText = [Text.Encoding]::UTF8.GetString($eventHeaderBefore)
    Assert-Format ($eventHeaderText -cmatch 'event_manifest_schema_version = 3U' -and
        $eventHeaderText -cmatch 'event_manifest_legacy_schema_version = 2U') 'R003_LEGACY_REGRESSION' 'manifest v3 compatibility constants changed'
    Assert-Format ((Get-Sha256Text ([IO.File]::ReadAllBytes($v2Path))) -ceq (Get-Sha256Text $v2BytesBefore) -and
        (Get-Sha256Text ([IO.File]::ReadAllBytes($eventHeaderPath))) -ceq (Get-Sha256Text $eventHeaderBefore)) 'R003_LEGACY_REGRESSION' 'legacy compatibility check wrote its inputs'
}

if ($Regenerate) {
    Write-GoldenFiles
}

$formatContract = [IO.File]::ReadAllText($formatContractPath, [Text.Encoding]::UTF8) | ConvertFrom-Json
Assert-Format ($formatContract.contractVersion -eq 1 -and $formatContract.format.formatVersion -eq 3 -and
    $formatContract.format.magicAsciiEscaped -ceq 'PBNVME3\0' -and
    $formatContract.format.headerBytes -eq 4096 -and $formatContract.format.indexEntryBytes -eq 160 -and
    $formatContract.format.footerBytes -eq 4096 -and $formatContract.integrity.checkValueHex -ceq 'E3069283') 'R003_CONTRACT_INVALID' 'PBNVME3 machine contract differs from ADR-019'
$crcCheck = [Text.Encoding]::ASCII.GetBytes('123456789')
$expectedCrcCheck = [Convert]::ToUInt32('E3069283', 16)
Assert-Format ((Get-Crc32C $crcCheck) -eq $expectedCrcCheck) 'R003_CONTRACT_INVALID' 'CRC32C implementation check failed'

$validated = @{}
foreach ($name in @('minimal', 'multi-frame', 'incomplete-frame', 'uncorrected-time')) {
    $directory = Join-Path $goldenRoot $name
    $blockPath = Join-Path $directory 'block.pbnvme3'
    $manifestPath = Join-Path $directory 'manifest-v4.json'
    Assert-Format ([IO.File]::Exists($blockPath) -and [IO.File]::Exists($manifestPath)) 'R003_GOLDEN_MISSING' "$name golden files are absent; run -Regenerate intentionally"
    $blockBytes = [IO.File]::ReadAllBytes($blockPath)
    $manifestBytes = [IO.File]::ReadAllBytes($manifestPath)
    $summary = Test-Pbnvme3Block $blockBytes $name
    $manifest = Test-ManifestV4 $manifestBytes $blockBytes $summary $name
    $validated[$name] = [pscustomobject]@{ block = $blockBytes; manifestBytes = $manifestBytes; summary = $summary; manifest = $manifest }
}

$base = $validated['minimal']
$corruptHeader = Copy-Bytes $base.block
$corruptHeader[200] = $corruptHeader[200] -bxor 1
Expect-FormatFailure { Test-Pbnvme3Block $corruptHeader 'corrupt-header' | Out-Null } 'NVME_BLOCK_CORRUPT' 'corrupt header'
$corruptIndex = Copy-Bytes $base.block
$corruptIndex[4100] = $corruptIndex[4100] -bxor 1
Expect-FormatFailure { Test-Pbnvme3Block $corruptIndex 'corrupt-index' | Out-Null } 'NVME_BLOCK_CORRUPT' 'corrupt index'
$corruptPayload = Copy-Bytes $base.block
$corruptPayload[8192] = $corruptPayload[8192] -bxor 1
Expect-FormatFailure { Test-Pbnvme3Block $corruptPayload 'corrupt-payload' | Out-Null } 'NVME_BLOCK_CORRUPT' 'corrupt payload'
[byte[]]$truncated = New-Object byte[] ($base.block.Length - 8)
[Array]::Copy($base.block, $truncated, $truncated.Length)
Expect-FormatFailure { Test-Pbnvme3Block $truncated 'truncated-footer' | Out-Null } 'NVME_BLOCK_INCOMPLETE' 'missing footer marker'
$futureBlock = Copy-Bytes $base.block
Set-U16 $futureBlock 8 4
Expect-FormatFailure { Test-Pbnvme3Block $futureBlock 'future-block' | Out-Null } 'NVME_FORMAT_UNSUPPORTED' 'future block version'
$outOfBounds = Copy-Bytes $base.block
Set-U64 $outOfBounds (4096 + 80) ([uint64]($base.block.Length - 4096))
Set-U32 $outOfBounds (4096 + 116) (Get-Crc32C $outOfBounds 4096 160 (4096 + 116) 4)
$footerStart = $outOfBounds.Length - 4096
Set-U32 $outOfBounds ($footerStart + 56) (Get-Crc32C $outOfBounds 4096 160)
Set-U32 $outOfBounds ($footerStart + 4084) (Get-Crc32C $outOfBounds $footerStart 4096 ($footerStart + 4084) 4)
Expect-FormatFailure { Test-Pbnvme3Block $outOfBounds 'out-of-bounds' | Out-Null } 'NVME_BLOCK_CORRUPT' 'out-of-bounds data range'

$futureManifest = [Text.Encoding]::UTF8.GetString($base.manifestBytes) | ConvertFrom-Json
$futureManifest.schemaVersion = 5
$futureManifestBytes = [Text.Encoding]::UTF8.GetBytes(($futureManifest | ConvertTo-Json -Depth 20))
Expect-FormatFailure { Test-ManifestV4 $futureManifestBytes $base.block $base.summary 'future-manifest' | Out-Null } 'EVENT_SCHEMA_UNSUPPORTED' 'future manifest version'
$badHashManifest = [Text.Encoding]::UTF8.GetString($base.manifestBytes) | ConvertFrom-Json
$badHashManifest.rawBlocks[0].sha256 = 'sha256:' + ('0' * 64)
$badHashBytes = [Text.Encoding]::UTF8.GetBytes(($badHashManifest | ConvertTo-Json -Depth 20))
Expect-FormatFailure { Test-ManifestV4 $badHashBytes $base.block $base.summary 'bad-hash' | Out-Null } 'EVENT_CHECKSUM_FAILED' 'manifest SHA mismatch'

Test-LegacyContracts
Write-Output 'R0-03 format validation passed: 4 golden scenarios, corruption/truncation/bounds/version/SHA rejection, and read-only v2/v3 compatibility.'
