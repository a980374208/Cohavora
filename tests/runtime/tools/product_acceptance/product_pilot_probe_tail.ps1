# Bounded shared UTF-8 suffix reader for the native acceptance probe.
# Read the final physical candidates from one fixed EOF snapshot. JSON parsing,
# identity checks, stale checks, and loss checks remain the caller's responsibility.
function Read-ProductPilotProbeTail {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [ValidateNotNullOrEmpty()]
        [string]$Path,

        [ValidateRange(1, 64)]
        [int]$Count = 3,

        [ValidateRange(1, 1048576)]
        [int]$MaximumBytes = 1048576
    )

    $stream = $null
    $decoded = New-Object 'System.Collections.Generic.List[string]'
    try {
        $share = [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete
        $stream = [System.IO.File]::Open(
            $Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, $share)
        [long]$snapshotEnd = $stream.Length
        if ($snapshotEnd -eq 0) { return }

        [int]$capacity = [Math]::Min([long]$MaximumBytes, $snapshotEnd)
        $buffer = New-Object byte[] $capacity
        [int]$bytesRead = 0
        [int]$lineFeeds = 0
        [int]$candidateStart = -1
        $endsWithLineFeed = $false

        while ($bytesRead -lt $capacity) {
            [int]$chunkLength = [Math]::Min(65536, $capacity - $bytesRead)
            [int]$chunkStart = $capacity - $bytesRead - $chunkLength
            [long]$fileStart = $snapshotEnd - $bytesRead - $chunkLength
            [void]$stream.Seek($fileStart, [System.IO.SeekOrigin]::Begin)
            [int]$chunkRead = 0
            while ($chunkRead -lt $chunkLength) {
                [int]$read = $stream.Read(
                    $buffer, $chunkStart + $chunkRead, $chunkLength - $chunkRead)
                if ($read -eq 0) {
                    throw [System.IO.EndOfStreamException]::new(
                        'NATIVE_PROBE_TAIL_SNAPSHOT_TRUNCATED')
                }
                $chunkRead += $read
            }

            if ($bytesRead -eq 0) {
                $endsWithLineFeed = ($buffer[$capacity - 1] -eq 10)
            }
            [int]$requiredLineFeeds = $Count
            if ($endsWithLineFeed) { $requiredLineFeeds++ }
            [int]$scanEnd = $capacity - $bytesRead - 1
            $bytesRead += $chunkLength
            for ($index = $scanEnd; $index -ge $chunkStart; $index--) {
                if ($buffer[$index] -ne 10) { continue }
                $lineFeeds++
                if ($lineFeeds -eq $requiredLineFeeds) {
                    $candidateStart = $index + 1
                    break
                }
            }
            if ($candidateStart -ge 0) { break }
            if ($fileStart -eq 0) {
                $candidateStart = $chunkStart
                break
            }
        }

        if ($candidateStart -lt 0) {
            throw [System.IO.InvalidDataException]::new('NATIVE_PROBE_TAIL_WINDOW_EXCEEDED')
        }
        [int]$contentEnd = $capacity
        if ($endsWithLineFeed) { $contentEnd-- }
        [long]$windowFileStart = $snapshotEnd - $capacity
        $strictUtf8 = [System.Text.UTF8Encoding]::new($false, $true)
        [int]$cursor = $candidateStart
        while ($cursor -le $contentEnd) {
            [int]$recordEnd = $cursor
            while ($recordEnd -lt $contentEnd -and $buffer[$recordEnd] -ne 10) {
                $recordEnd++
            }
            $hasDelimiter = ($recordEnd -lt $contentEnd)
            $recordTerminated = $hasDelimiter -or $endsWithLineFeed
            [int]$decodeStart = $cursor
            [int]$decodeEnd = $recordEnd
            if ($recordTerminated -and $decodeEnd -gt $decodeStart -and
                $buffer[$decodeEnd - 1] -eq 13) {
                $decodeEnd--
            }

            # Only the BOM at the physical file start is an encoding marker.
            $hasInitialBom = (($windowFileStart + $decodeStart) -eq 0 -and
                ($decodeEnd - $decodeStart) -ge 3 -and
                $buffer[$decodeStart] -eq 239 -and
                $buffer[$decodeStart + 1] -eq 187 -and
                $buffer[$decodeStart + 2] -eq 191)
            if ($hasInitialBom) { $decodeStart += 3 }
            if ($hasInitialBom -and -not $recordTerminated -and
                $decodeStart -eq $decodeEnd -and $snapshotEnd -eq 3) {
                break
            }

            # An append snapshot may end part-way through a valid UTF-8 sequence.
            # Exclude only that physical candidate; do not replace it with an older
            # line. Strictly decode its prefix so other malformed bytes still fail.
            [int]$incompleteStart = -1
            if (-not $recordTerminated -and $decodeEnd -gt $decodeStart) {
                [int]$lead = $decodeEnd - 1
                while ($lead -ge $decodeStart -and
                    $buffer[$lead] -ge 128 -and $buffer[$lead] -le 191) {
                    $lead--
                }
                if ($lead -ge $decodeStart) {
                    [int]$leadByte = $buffer[$lead]
                    [int]$expected = 0
                    if ($leadByte -ge 194 -and $leadByte -le 223) { $expected = 2 }
                    elseif ($leadByte -ge 224 -and $leadByte -le 239) { $expected = 3 }
                    elseif ($leadByte -ge 240 -and $leadByte -le 244) { $expected = 4 }
                    [int]$available = $decodeEnd - $lead
                    if ($expected -gt $available) {
                        $validPrefix = $true
                        if ($available -gt 1) {
                            [int]$secondByte = $buffer[$lead + 1]
                            if (($leadByte -eq 224 -and $secondByte -lt 160) -or
                                ($leadByte -eq 237 -and $secondByte -gt 159) -or
                                ($leadByte -eq 240 -and $secondByte -lt 144) -or
                                ($leadByte -eq 244 -and $secondByte -gt 143)) {
                                $validPrefix = $false
                            }
                        }
                        if ($validPrefix) { $incompleteStart = $lead }
                    }
                }
            }

            if ($incompleteStart -ge 0) {
                [void]$strictUtf8.GetString(
                    $buffer, $decodeStart, $incompleteStart - $decodeStart)
            }
            else {
                $record = $strictUtf8.GetString(
                    $buffer, $decodeStart, $decodeEnd - $decodeStart)
                [void]$decoded.Add($record)
            }
            if (-not $hasDelimiter) { break }
            $cursor = $recordEnd + 1
        }
    }
    finally {
        if ($null -ne $stream) { $stream.Dispose() }
    }

    # Decode the entire selection before emitting, avoiding a partial result on
    # malformed UTF-8. Empty strings remain physical candidates in the pipeline.
    foreach ($record in $decoded) { Write-Output -NoEnumerate $record }
}

# Action/resource JSONL readers require a newline-complete record. Unlike the
# native probe reader above, an unterminated EOF candidate is always excluded
# and the preceding complete record is selected. JSON/schema validation remains
# the caller's responsibility; malformed selected content must not be skipped.
function Read-ProductPilotCompleteJsonlTail {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [ValidateNotNullOrEmpty()]
        [string]$Path,

        [ValidateRange(1, 64)]
        [int]$Count = 1,

        [ValidateRange(1, 1048576)]
        [int]$MaximumBytes = 1048576
    )

    $stream = $null
    $decoded = New-Object 'System.Collections.Generic.List[string]'
    try {
        $share = [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete
        $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, $share)
        [long]$snapshotEnd = $stream.Length
        if ($snapshotEnd -eq 0) { return }
        [int]$capacity = [Math]::Min([long]$MaximumBytes, $snapshotEnd)
        $buffer = New-Object byte[] $capacity
        [int]$bytesRead = 0
        [int]$lineFeeds = 0
        [int]$candidateStart = -1
        [int]$contentEnd = -1
        while ($bytesRead -lt $capacity) {
            [int]$chunkLength = [Math]::Min(65536, $capacity - $bytesRead)
            [int]$chunkStart = $capacity - $bytesRead - $chunkLength
            [long]$fileStart = $snapshotEnd - $bytesRead - $chunkLength
            [void]$stream.Seek($fileStart, [IO.SeekOrigin]::Begin)
            [int]$chunkRead = 0
            while ($chunkRead -lt $chunkLength) {
                [int]$read = $stream.Read($buffer, $chunkStart + $chunkRead, $chunkLength - $chunkRead)
                if ($read -eq 0) {
                    throw [IO.EndOfStreamException]::new('COMPLETE_JSONL_TAIL_SNAPSHOT_TRUNCATED')
                }
                $chunkRead += $read
            }
            [int]$scanEnd = $capacity - $bytesRead - 1
            $bytesRead += $chunkLength
            for ($index = $scanEnd; $index -ge $chunkStart; $index--) {
                if ($buffer[$index] -ne 10) { continue }
                $lineFeeds++
                if ($contentEnd -lt 0) { $contentEnd = $index }
                if ($lineFeeds -eq ($Count + 1)) {
                    $candidateStart = $index + 1
                    break
                }
            }
            if ($candidateStart -ge 0) { break }
            if ($fileStart -eq 0) {
                $candidateStart = $chunkStart
                break
            }
        }
        if ($candidateStart -lt 0) {
            throw [IO.InvalidDataException]::new('COMPLETE_JSONL_TAIL_WINDOW_EXCEEDED')
        }
        if ($contentEnd -lt 0) { return }
        [long]$windowFileStart = $snapshotEnd - $capacity
        $strictUtf8 = [Text.UTF8Encoding]::new($false, $true)
        [int]$cursor = $candidateStart
        while ($cursor -le $contentEnd) {
            [int]$recordEnd = $cursor
            while ($recordEnd -lt $contentEnd -and $buffer[$recordEnd] -ne 10) { $recordEnd++ }
            [int]$decodeStart = $cursor
            [int]$decodeEnd = $recordEnd
            if ($decodeEnd -gt $decodeStart -and $buffer[$decodeEnd - 1] -eq 13) { $decodeEnd-- }
            if (($windowFileStart + $decodeStart) -eq 0 -and
                ($decodeEnd - $decodeStart) -ge 3 -and
                $buffer[$decodeStart] -eq 239 -and $buffer[$decodeStart + 1] -eq 187 -and
                $buffer[$decodeStart + 2] -eq 191) { $decodeStart += 3 }
            [void]$decoded.Add($strictUtf8.GetString($buffer, $decodeStart, $decodeEnd - $decodeStart))
            if ($recordEnd -eq $contentEnd) { break }
            $cursor = $recordEnd + 1
        }
    } finally {
        if ($stream) { $stream.Dispose() }
    }
    foreach ($record in $decoded) { Write-Output -NoEnumerate $record }
}
