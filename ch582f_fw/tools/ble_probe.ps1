# Connect to a BLE device by address and dump GATT services (probe for 0x1812 etc).
# This version FORCES a real connection:
#   - opens a GattSession with MaintainConnection = true (triggers a link),
#   - enumerates services with BluetoothCacheMode.Uncached (must talk to the peer).
# If the peer is NOT really connectable, this reports Disconnected + gatt error.
# NOTE: keep this file ASCII-only (PS 5.1 mis-parses UTF-8-no-BOM CJK comments).
param(
    [Parameter(Mandatory = $true)][string]$Addr   # e.g. c2:53:10:8c:eb:3d
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Runtime.WindowsRuntime

[void][Windows.Devices.Enumeration.DeviceInformation,Windows.Devices.Enumeration,ContentType=WindowsRuntime]
[void][Windows.Devices.Bluetooth.BluetoothLEDevice,Windows.Devices.Bluetooth,ContentType=WindowsRuntime]
[void][Windows.Devices.Bluetooth.BluetoothConnectionStatus,Windows.Devices.Bluetooth,ContentType=WindowsRuntime]
[void][Windows.Devices.Bluetooth.BluetoothCacheMode,Windows.Devices.Bluetooth,ContentType=WindowsRuntime]
[void][Windows.Devices.Bluetooth.GenericAttributeProfile.GattSession,Windows.Devices.Bluetooth.GenericAttributeProfile,ContentType=WindowsRuntime]
[void][Windows.Devices.Bluetooth.GenericAttributeProfile.GattDeviceService,Windows.Devices.Bluetooth.GenericAttributeProfile,ContentType=WindowsRuntime]
[void][Windows.Devices.Bluetooth.GenericAttributeProfile.GattDeviceServicesResult,Windows.Devices.Bluetooth.GenericAttributeProfile,ContentType=WindowsRuntime]

$asTask = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
        $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
        $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
    })[0]

function Await($op, $t) {
    $m = $asTask.MakeGenericMethod($t)
    $task = $m.Invoke($null, @($op))
    $task.Wait(-1) | Out-Null
    $task.Result
}

# Parse "c2:53:10:8c:eb:3d" -> UInt64 (MSB-first, as displayed) e.g. 0xC253108CEB3D
$parts = $Addr.Split(':')
if ($parts.Count -ne 6) { throw "bad address: $Addr" }
[uint64]$mac = 0
for ($i = 0; $i -lt 6; $i++) { $mac = ($mac -shl 8) -bor [Convert]::ToUInt32($parts[$i], 16) }
Write-Host ("addr parsed: {0:X12}" -f $mac)

$dev = Await ([Windows.Devices.Bluetooth.BluetoothLEDevice]::FromBluetoothAddressAsync([uint64]$mac)) ([Windows.Devices.Bluetooth.BluetoothLEDevice])
if ($null -eq $dev) { throw "FromBluetoothAddressAsync returned null (not advertising / not in range?)" }
Write-Host "device name : $($dev.Name)"
Write-Host "conn status : $($dev.ConnectionStatus)"

# --- Force a real link: GattSession with MaintainConnection = true ---
$session = $null
try {
    $session = Await ($dev.RequestSessionAsync()) ([Windows.Devices.Bluetooth.GenericAttributeProfile.GattSession])
    if ($session) {
        Write-Host "session     : got (id=$($session.DeviceId.Id))"
        $session.MaintainConnection = $true
        Write-Host "maintain    : $($session.MaintainConnection)"
    } else {
        Write-Host "session     : NULL (no session returned)"
    }
} catch {
    Write-Host "session     : ERROR $($_.Exception.Message)"
}

# Poll connection status up to ~15s
for ($i = 0; $i -lt 30; $i++) {
    if ($dev.ConnectionStatus -eq [Windows.Devices.Bluetooth.BluetoothConnectionStatus]::Connected) { break }
    Start-Sleep -Milliseconds 500
}
Write-Host "conn status(2): $($dev.ConnectionStatus)"
Write-Host "is paired    : $($dev.DeviceInformation.Pairing.IsPaired)"

# --- Uncached enumeration: must actually talk to the peer ---
Write-Host "--- GATT (cached) ---"
try {
    $cached = Await ($dev.GetGattServicesAsync([Windows.Devices.Bluetooth.BluetoothCacheMode]::Cached)) ([Windows.Devices.Bluetooth.GenericAttributeProfile.GattDeviceServicesResult])
    Write-Host "  status=$($cached.Status) count=$($cached.Services.Count)"
    foreach ($s in $cached.Services) { Write-Host ("    svc {0}" -f $s.Uuid) }
} catch {
    Write-Host "  ERR $($_.Exception.Message)"
}

Write-Host "--- GATT (uncached, real connection) ---"
try {
    $res = Await ($dev.GetGattServicesAsync([Windows.Devices.Bluetooth.BluetoothCacheMode]::Uncached)) ([Windows.Devices.Bluetooth.GenericAttributeProfile.GattDeviceServicesResult])
    Write-Host "  status=$($res.Status) count=$($res.Services.Count)"
    foreach ($s in $res.Services) { Write-Host ("    svc {0}" -f $s.Uuid) }
} catch {
    Write-Host "  ERR $($_.Exception.Message)"
}

Write-Host "final conn  : $($dev.ConnectionStatus)"
if ($session) { $session.Dispose() }
