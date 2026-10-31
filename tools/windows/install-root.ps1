#Requires -Version 7.0
param([string] $Image = (Join-Path $PSScriptRoot 'release.img'))
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'RootPlan.psm1') -Force

# Windows Storage/BitLocker and physical-device I/O remain unverified here:
# not verified on X79-VG4 hardware. Host tests substitute only these boundaries.
function Assert-InstallHost {
    if (-not $IsWindows) { throw 'Run this installer on Windows with PowerShell 7.' }
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run PowerShell as Administrator.'
    }
}

function Get-InstallLogPath {
    $desktop = [Environment]::GetFolderPath('Desktop')
    if ([string]::IsNullOrWhiteSpace($desktop)) { throw 'Cannot locate Desktop for install log.' }
    return (Join-Path $desktop 'logitos-install.txt')
}

function Get-RootInventory {
    # Get-Disk EXCLUDES dynamic disks (Microsoft's documented behavior), so an
    # absent IsDynamic property must never be interpreted as "basic".
    # https://learn.microsoft.com/powershell/module/storage/get-disk
    # https://learn.microsoft.com/windows/win32/cimwin32prov/win32-diskpartition
    $legacy = @(Get-CimInstance -ClassName Win32_DiskPartition -ErrorAction Stop)
    if (@($legacy | Where-Object { $_.Type -match 'Logical Disk Manager|LDM' }).Count -ne 0) {
        throw 'dynamic-disk: LDM partitions present; refusing incomplete Storage inventory.'
    }
    $disks = @(Get-Disk -ErrorAction Stop | ForEach-Object {
        [pscustomobject]@{ Number=$_.Number; Size=[long]$_.Size; UniqueId=[string]$_.UniqueId;
            PartitionStyle=[string]$_.PartitionStyle; LogicalSectorSize=$_.LogicalSectorSize;
            PhysicalSectorSize=$_.PhysicalSectorSize; IsOffline=[bool]$_.IsOffline;
            IsReadOnly=[bool]$_.IsReadOnly; IsDynamic=$false }
    })
    $parts = @(Get-Partition -ErrorAction Stop | ForEach-Object {
        [pscustomobject]@{ DiskNumber=$_.DiskNumber; PartitionNumber=$_.PartitionNumber;
            Offset=[long]$_.Offset; Size=[long]$_.Size;
            DriveLetter=([string]$_.DriveLetter).Trim([char]0);
            GptType=[string]$_.GptType; AccessPaths=@($_.AccessPaths) }
    })
    $volumes = @(Get-Volume -ErrorAction Stop | ForEach-Object {
        [pscustomobject]@{ DriveLetter=([string]$_.DriveLetter).Trim([char]0);
            FileSystem=[string]$_.FileSystem; BitLockerVolumeStatus='';
            BitLockerProtectionStatus=''; BitLockerLockStatus='' }
    })
    # Unavailable BitLocker tooling/metadata is a refusal, not "encryption off".
    # ProtectionStatus Off alone includes suspended protection.
    # https://learn.microsoft.com/powershell/module/bitlocker/get-bitlockervolume
    $crypto = @(Get-BitLockerVolume -MountPoint 'C:' -ErrorAction Stop)
    if ($crypto.Count -ne 1) { throw 'bitlocker-unknown: C: status is not unique.' }
    foreach ($volume in $volumes) {
        if ($volume.DriveLetter -eq 'C') {
            $volume.BitLockerVolumeStatus=[string]$crypto[0].VolumeStatus
            $volume.BitLockerProtectionStatus=[string]$crypto[0].ProtectionStatus
            $volume.BitLockerLockStatus=[string]$crypto[0].LockStatus
        }
    }
    return @{ Disks=$disks; Partitions=$parts; Volumes=$volumes }
}

function Get-InventoryStamp($Inventory) {
    # Do not fingerprint mutable free-space statistics: creating the partition
    # changes those. Disk identity and EVERY old partition boundary must agree.
    return (@{ Disks=@($Inventory.Disks | Sort-Object Number);
               Partitions=@($Inventory.Partitions | Sort-Object DiskNumber,PartitionNumber);
               Volumes=@($Inventory.Volumes | Where-Object DriveLetter -eq 'C') } |
        ConvertTo-Json -Depth 12 -Compress)
}

function Assert-NewRoot($Part, $Plan) {
    if ($Part.DiskNumber -ne $Plan.DiskNumber -or [long]$Part.Offset -ne $Plan.Offset -or
        [long]$Part.Size -ne $Plan.Size -or
        ([string]$Part.GptType).Trim('{}') -ne $LogitRootType.Trim('{}') -or
        $Part.PartitionNumber -le 0) { throw 'New partition does not exactly match approved disk/offset/size/type.' }
    if (([string]$Part.DriveLetter).Trim([char]0).Length -ne 0) {
        throw 'New partition unexpectedly has a drive letter.'
    }
    if (@($Part.AccessPaths | Where-Object { $_ -and $_ -notmatch '^\\\\\?\\Volume\{[^}]+\}\\$' }).Count -ne 0) {
        throw 'New partition unexpectedly has a mounted access path.'
    }
}

function Open-RootDevice([int] $DiskNumber, [IO.FileAccess] $Access) {
    # PhysicalDrive is the whole disk: the ONLY data write below follows an
    # explicit seek to the newly created partition. Never open C: or a letter.
    # Windows requires shared access to an in-use physical disk; it can refuse
    # raw writes into mounted volumes. Do not unlock/dismount the Windows disk
    # or bypass that protection to make an installation work.
    # https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-createfilew
    # https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-writefile
    return [IO.FileStream]::new("\\.\PhysicalDrive$DiskNumber", [IO.FileMode]::Open,
        $Access, [IO.FileShare]::ReadWrite, 1MB, [IO.FileOptions]::WriteThrough)
}

function Copy-RootImage([IO.Stream] $Source, [IO.Stream] $Target, [long] $Offset, [long] $Length) {
    [void]$Source.Seek(0, [IO.SeekOrigin]::Begin)
    [void]$Target.Seek($Offset, [IO.SeekOrigin]::Begin)
    $buffer = [byte[]]::new(1MB)
    $left = $Length
    while ($left -gt 0) {
        $count = $Source.Read($buffer, 0, [int][math]::Min([long]$buffer.Length, $left))
        if ($count -eq 0) { throw 'Image ended during write.' }
        # Never copy to EOF on a physical drive: this loop is bounded by the
        # image length validated BEFORE New-Partition changed the GPT.
        $Target.Write($buffer, 0, $count)
        $left -= $count
    }
    $Target.Flush()
}

function Get-RangeHash([IO.Stream] $Stream, [long] $Offset, [long] $Length) {
    [void]$Stream.Seek($Offset, [IO.SeekOrigin]::Begin)
    $hash = [Security.Cryptography.IncrementalHash]::CreateHash('SHA256')
    try {
        $buffer = [byte[]]::new(1MB)
        $left = $Length
        while ($left -gt 0) {
            $count = $Stream.Read($buffer, 0, [int][math]::Min([long]$buffer.Length, $left))
            if ($count -eq 0) { throw 'Short read during SHA-256 verification.' }
            $hash.AppendData($buffer, 0, $count)
            $left -= $count
        }
        return [Convert]::ToHexString($hash.GetHashAndReset())
    } finally { $hash.Dispose() }
}

function Invoke-LogitInstall([string] $Image) {
    Assert-InstallHost
    $log = Get-InstallLogPath
    $source = $null
    Start-Transcript -Path $log -Append -ErrorAction Stop | Out-Null
    try {
        Write-Host 'LogitOS root install; not verified on X79-VG4 hardware.'
        # Hold this read handle through both confirmations. On Windows sharing
        # Read denies concurrent writers/replacement of the approved image.
        $source = [IO.File]::Open($Image, 'Open', 'Read', 'Read')
        $bytes = $source.Length
        if ($bytes -le 0 -or ($bytes % 512) -ne 0) { throw 'Image must be nonempty and sector aligned.' }
        $expected = Get-RangeHash $source 0 $bytes
        $inventory = Get-RootInventory
        $plan = Get-LogitRootPlan @inventory -ImageBytes $bytes
        if (-not $plan.Ok) { throw "Root plan refused: $($plan.Reason)" }
        $stamp = Get-InventoryStamp $inventory
        Write-Host "Image $Image bytes=$bytes SHA-256=$expected"
        Write-Host "Create root on PhysicalDrive$($plan.DiskNumber) offset=$($plan.Offset) size=$($plan.Size) type=$LogitRootType"
        if ((Read-Host 'Type YES to create this partition') -cne 'YES') { throw 'Cancelled before partition creation.' }
        if ((Get-InventoryStamp (Get-RootInventory)) -cne $stamp) { throw 'Inventory changed before creation.' }
        # Omitting -AssignDriveLetter and ALL formatting cmdlets is deliberate.
        # https://learn.microsoft.com/powershell/module/storage/new-partition
        $created = @(New-Partition -DiskNumber $plan.DiskNumber -Offset $plan.Offset `
            -Size $plan.Size -GptType $LogitRootType -ErrorAction Stop)
        if ($created.Count -ne 1) { throw 'New-Partition did not return exactly one new partition.' }
        Assert-NewRoot $created[0] $plan
        $number = $created[0].PartitionNumber
        Write-Host "Created disk=$($plan.DiskNumber) partition=$number offset=$($plan.Offset) size=$($plan.Size)"
        # Plan requests YES before creation; spec additionally requires reviewing
        # the actual returned partition before raw writes. Two confirmations
        # meet both. Refusal leaves the new EMPTY partition for manual review;
        # never automatically delete/roll back a partition on the Windows disk.
        if ((Read-Host 'Type YES to write the image into this new partition') -cne 'YES') {
            throw 'Cancelled before raw write; empty partition retained.'
        }
        $fresh = Get-RootInventory
        $actual = @($fresh.Partitions | Where-Object {
            $_.DiskNumber -eq $plan.DiskNumber -and $_.PartitionNumber -eq $number
        })
        if ($actual.Count -ne 1) { throw 'New partition disappeared or is ambiguous.' }
        Assert-NewRoot $actual[0] $plan
        # Remove ONLY the partition this invocation created, then re-run the
        # full inventory policy: other disks, encryption and old extents cannot
        # change while the human is reviewing the confirmation.
        $fresh.Partitions = @($fresh.Partitions | Where-Object {
            -not ($_.DiskNumber -eq $plan.DiskNumber -and $_.PartitionNumber -eq $number)
        })
        if ((Get-InventoryStamp $fresh) -cne $stamp) { throw 'Inventory changed before raw write.' }
        $again = Get-LogitRootPlan @fresh -ImageBytes $bytes
        if (-not $again.Ok -or $again.Offset -ne $plan.Offset -or $again.Size -ne $plan.Size) {
            throw 'Root plan changed before raw write.'
        }
        # Get-Volume's -Partition expects the live CIM object, not our normalized
        # inventory record. Re-fetch it through Storage rather than coercing.
        $live = @(Get-Partition -DiskNumber $plan.DiskNumber -PartitionNumber $number -ErrorAction Stop)
        if ($live.Count -ne 1) { throw 'Live partition is not unique before raw write.' }
        Assert-NewRoot $live[0] $plan
        $volumes = @(Get-Volume -Partition $live[0] -ErrorAction Stop)
        if (@($volumes | Where-Object { $_.FileSystem -or ([string]$_.DriveLetter).Trim([char]0) }).Count -ne 0) {
            throw 'New partition has a filesystem or drive letter.'
        }
        $device = Open-RootDevice $plan.DiskNumber ([IO.FileAccess]::ReadWrite)
        try {
            Copy-RootImage $source $device $plan.Offset $bytes
            $device.Flush($true)
        } finally { $device.Dispose() }
        # Close and reopen so verification is a distinct device read. This is
        # readback evidence, not a claim of power-loss durability/controller flush.
        $device = Open-RootDevice $plan.DiskNumber ([IO.FileAccess]::Read)
        try { $actualHash = Get-RangeHash $device $plan.Offset $bytes }
        finally { $device.Dispose() }
        if ($actualHash -cne $expected) { throw "SHA-256 mismatch: image=$expected readback=$actualHash" }
        Write-Host "SHA-256 verified: $actualHash; disk=$($plan.DiskNumber) partition=$number bytes=$bytes"
    } catch {
        Write-Host "INSTALL STOPPED: $($_.Exception.Message)"
        throw
    } finally {
        if ($null -ne $source) { $source.Dispose() }
        Stop-Transcript | Out-Null
    }
}

# Dot-sourcing loads the functions for a host boundary test or review without
# executing installation; normal -File invocation always follows the full flow.
if ($MyInvocation.InvocationName -ne '.') { Invoke-LogitInstall -Image $Image }
