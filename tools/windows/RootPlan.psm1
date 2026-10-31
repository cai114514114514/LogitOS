# Pure inventory policy: no Storage, CIM, BitLocker, or file I/O in this module.
# Model evidence only: not verified on X79-VG4 hardware.
Set-StrictMode -Version Latest

# Authoritative installer constant. The kernel currently discovers LogitFS by
# superblock, not by type GUID; never substitute the Windows basic-data GUID.
$LogitRootType = '{6c6f6769-7466-4a73-9a2b-58373956b434}'

function Read-Field($Object, [string] $Name) {
    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -ne $property) { return $property.Value }
    return $null
}

function New-RootResult([string] $Reason, $DiskNumber = $null, $Offset = $null, $Size = $null) {
    return @{ Ok=($Reason -eq 'ok'); Reason=$Reason; DiskNumber=$DiskNumber; Offset=$Offset; Size=$Size }
}

function Align-Up([long] $Value) {
    return [long]([math]::Ceiling([decimal]$Value / 1MB) * 1MB)
}

function Test-RootOverlap([object[]] $Parts) {
    $end = 0L
    foreach ($part in @($Parts | Sort-Object Offset)) {
        if ([long]$part.Offset -lt $end) { return $true }
        $end = [long]$part.Offset + [long]$part.Size
    }
    return $false
}

function Get-LogitRootPlan {
    <# Inventory contract (all byte counts are integers):
       Disks: Number, Size, LogicalSectorSize, PhysicalSectorSize, PartitionStyle,
              IsDynamic (collector-enriched), IsOffline, IsReadOnly, UniqueId.
       Partitions: DiskNumber, PartitionNumber, Offset, Size, DriveLetter, GptType.
       Volumes: DriveLetter, FileSystem, BitLockerVolumeStatus,
                BitLockerProtectionStatus, BitLockerLockStatus (collector-enriched).
       Missing encryption/dynamic status is uncertainty, never permission to write.
       Windows does not enumerate free-space partitions: derive gaps from ALL
       allocated partitions, including hidden/recovery partitions, not Volume.Size.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [AllowEmptyCollection()] [object[]] $Disks,
        [Parameter(Mandatory)] [AllowEmptyCollection()] [object[]] $Partitions,
        [Parameter(Mandatory)] [AllowEmptyCollection()] [object[]] $Volumes,
        [Parameter(Mandatory)] [long] $ImageBytes
    )
    if ($ImageBytes -le 0 -or $Disks.Count -eq 0) { return (New-RootResult 'invalid-inventory') }
    try {
        $numbers = @{}
        foreach ($disk in $Disks) {
            foreach ($field in 'Number','Size','LogicalSectorSize','PhysicalSectorSize',
                     'PartitionStyle','IsDynamic','IsOffline','IsReadOnly','UniqueId') {
                if ($null -eq (Read-Field $disk $field)) { return (New-RootResult 'invalid-inventory') }
            }
            $key = [string]$disk.Number
            if ($numbers.ContainsKey($key) -or [long]$disk.Number -lt 0 -or
                [decimal]$disk.Size -gt ([decimal][long]::MaxValue - 2MB) -or
                [long]$disk.Size -lt 2MB -or [string]::IsNullOrWhiteSpace($disk.UniqueId)) {
                return (New-RootResult 'invalid-inventory')
            }
            # The target Netac is 512/512. Refuse other geometries until raw
            # write alignment and GPT bounds are tested for them on Windows.
            if ($disk.LogicalSectorSize -ne 512 -or $disk.PhysicalSectorSize -ne 512) {
                return (New-RootResult 'unsupported-sector-size')
            }
            $numbers[$key] = $disk
        }
        $identities = @{}
        foreach ($part in $Partitions) {
            foreach ($field in 'DiskNumber','PartitionNumber','Offset','Size','DriveLetter','GptType') {
                if ($null -eq (Read-Field $part $field)) { return (New-RootResult 'invalid-inventory') }
            }
            $disk = $numbers[[string]$part.DiskNumber]
            $key = "$($part.DiskNumber):$($part.PartitionNumber)"
            if ($null -eq $disk -or $identities.ContainsKey($key) -or
                [long]$part.Offset -lt 1MB -or [long]$part.Size -le 0 -or
                ([long]$part.Offset % 512) -ne 0 -or ([long]$part.Size % 512) -ne 0 -or
                ([decimal]$part.Offset + [decimal]$part.Size) -gt ([decimal]$disk.Size - 33*512)) {
                return (New-RootResult 'invalid-inventory')
            }
            $identities[$key] = $true
        }
    } catch { return (New-RootResult 'invalid-inventory') }

    $windows = @($Partitions | Where-Object { [string]$_.DriveLetter -eq 'C' })
    if ($windows.Count -ne 1) { return (New-RootResult 'c-partition-not-unique') }
    $c = $windows[0]
    $target = $numbers[[string]$c.DiskNumber]
    if ($target.IsDynamic) { return (New-RootResult 'dynamic-disk') }
    if ($target.PartitionStyle -ne 'GPT') { return (New-RootResult 'not-gpt') }
    if ($target.IsOffline -or $target.IsReadOnly) { return (New-RootResult 'disk-unavailable') }
    $volume = @($Volumes | Where-Object { [string]$_.DriveLetter -eq 'C' })
    if ($volume.Count -ne 1) { return (New-RootResult 'bitlocker-unknown') }
    foreach ($field in 'BitLockerVolumeStatus','BitLockerProtectionStatus','BitLockerLockStatus') {
        if ([string]::IsNullOrWhiteSpace((Read-Field $volume[0] $field))) {
            return (New-RootResult 'bitlocker-unknown')
        }
    }
    # Protection Off alone also means "suspended"; encrypted bytes are still
    # encrypted. Require fully decrypted AND unlocked, not just disabled keys.
    if ($volume[0].BitLockerVolumeStatus -ne 'FullyDecrypted' -or
        $volume[0].BitLockerProtectionStatus -ne 'Off' -or
        $volume[0].BitLockerLockStatus -ne 'Unlocked') { return (New-RootResult 'bitlocker') }

    $gaps = @()
    foreach ($disk in $Disks) {
        $parts = @($Partitions | Where-Object { $_.DiskNumber -eq $disk.Number } | Sort-Object Offset)
        if (Test-RootOverlap $parts) { return (New-RootResult 'partition-overlap') }
        if (@($parts | Where-Object {
            ([string]$_.GptType).Trim('{}') -eq $LogitRootType.Trim('{}')
        }).Count -ne 0) {
            return (New-RootResult 'root-already-present')
        }
        # Leave the GPT entry array and header at each end. Ignore sub-MiB
        # alignment slivers, but count every usable MiB gap even if too small
        # for this image: picking the larger of two gaps is an unsafe guess.
        $cursor = 1MB
        $limit = [long]([math]::Floor(([decimal]$disk.Size - 33*512) / 1MB) * 1MB)
        $ends = @($parts) + @([pscustomobject]@{ Offset=$limit; Size=0 })
        foreach ($part in $ends) {
            $start = Align-Up $cursor
            $end = [long]([math]::Floor([decimal]$part.Offset / 1MB) * 1MB)
            if ($end - $start -ge 1MB) {
                $gaps += [pscustomobject]@{ DiskNumber=$disk.Number; Offset=$start; Size=($end-$start) }
            }
            $cursor = [math]::Max([long]$cursor, ([long]$part.Offset + [long]$part.Size))
        }
    }
    if (@($gaps | Select-Object -ExpandProperty DiskNumber -Unique).Count -gt 1) {
        return (New-RootResult 'multiple-candidate-disks')
    }
    if ($gaps.Count -gt 1) { return (New-RootResult 'multiple-free-extents') }
    if ($gaps.Count -eq 0) { return (New-RootResult 'no-free-extent') }
    $gap = $gaps[0]
    if ($gap.DiskNumber -ne $c.DiskNumber -or $gap.Offset -ne (Align-Up ($c.Offset + $c.Size))) {
        return (New-RootResult 'free-extent-not-after-c')
    }
    if ($gap.Size -lt $ImageBytes) { return (New-RootResult 'image-too-large') }
    return (New-RootResult 'ok' $gap.DiskNumber $gap.Offset $gap.Size)
}

Export-ModuleMember -Function Get-LogitRootPlan -Variable LogitRootType
