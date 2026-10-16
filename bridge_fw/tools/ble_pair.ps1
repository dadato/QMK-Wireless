# Pair the keyboard module BLE device on Windows (WinRT DevicePairing).
# Strategy A: plain Pairing.PairAsync()  (system handles the ceremony; no app handler).
# Strategy B: Custom.PairAsync() + PairingRequested handler (file-logged so we can tell
#             whether the ceremony callback actually fires).
# NOTE: keep this file ASCII-only (PS 5.1 mis-parses UTF-8-no-BOM CJK comments).
param(
    [Parameter(Mandatory = $true)][string]$Addr,   # e.g. c2:53:10:8c:eb:3d
    [switch]$SkipPlain
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Runtime.WindowsRuntime

[void][Windows.Devices.Enumeration.DeviceInformation,Windows.Devices.Enumeration,ContentType=WindowsRuntime]
[void][Windows.Devices.Enumeration.DeviceInformationCustomPairing,Windows.Devices.Enumeration,ContentType=WindowsRuntime]
[void][Windows.Devices.Enumeration.DevicePairingKinds,Windows.Devices.Enumeration,ContentType=WindowsRuntime]
[void][Windows.Devices.Enumeration.DevicePairingResult,Windows.Devices.Enumeration,ContentType=WindowsRuntime]
[void][Windows.Devices.Enumeration.DevicePairingRequestedEventArgs,Windows.Devices.Enumeration,ContentType=WindowsRuntime]
[void][Windows.Devices.Bluetooth.BluetoothLEDevice,Windows.Devices.Bluetooth,ContentType=WindowsRuntime]

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

$addrLc = $Addr.ToLower()
$parts = $Addr.Split(':')
if ($parts.Count -ne 6) { throw "bad address: $Addr" }
[uint64]$mac = 0
for ($i = 0; $i -lt 6; $i++) { $mac = ($mac -shl 8) -bor [Convert]::ToUInt32($parts[$i], 16) }
Write-Host ("addr parsed : {0:X12}" -f $mac)

# --- enumerate the live (unpaired) BLE device list and find ours ---
$q = [Windows.Devices.Bluetooth.BluetoothLEDevice]::GetDeviceSelectorFromPairingState($false)
$devs = Await ([Windows.Devices.Enumeration.DeviceInformation]::FindAllAsync($q)) ([Windows.Devices.Enumeration.DeviceInformationCollection])
Write-Host "unpaired    : $($devs.Count) device(s)"
$di = $null
foreach ($d in $devs) {
    $hit = $d.Id.ToLower().Contains($addrLc)
    Write-Host ("  {0,-34} {1}{2}" -f $d.Name, $d.Id, $(if ($hit) { '  <== target' } else { '' }))
    if ($hit) { $di = $d }
}
if ($null -eq $di) {
    Write-Host "not found in unpaired list; falling back to FromBluetoothAddressAsync"
    $dev = Await ([Windows.Devices.Bluetooth.BluetoothLEDevice]::FromBluetoothAddressAsync([uint64]$mac)) ([Windows.Devices.Bluetooth.BluetoothLEDevice])
    if ($null -ne $dev) { $di = $dev.DeviceInformation }
}
if ($null -eq $di) { throw "device not found" }

Write-Host "name        : $($di.Name)"
Write-Host "id          : $($di.Id)"
Write-Host "canPair     : $($di.Pairing.CanPair)"
Write-Host "isPaired    : $($di.Pairing.IsPaired)"
Write-Host "protection  : $($di.Pairing.ProtectionLevel)"

if ($di.Pairing.IsPaired) { Write-Host 'ALREADY-PAIRED'; exit 0 }

if (-not $SkipPlain) {
    Write-Host "--- strategy A: plain PairAsync (system ceremony) ---"
    $ra = Await ($di.Pairing.PairAsync()) ([Windows.Devices.Enumeration.DevicePairingResult])
    Write-Host "plain result: status=$($ra.Status) protection=$($ra.ProtectionLevelUsed)"
    Write-Host "isPaired    : $($di.Pairing.IsPaired)"
    if ($di.Pairing.IsPaired) { Write-Host 'PAIRED-OK(plain)'; exit 0 }
}

# --- strategy B: Custom.PairAsync + ceremony handler, logged to a file ---
Write-Host "--- strategy B: custom PairAsync + ceremony handler ---"
$script:pairLog = Join-Path $env:TEMP 'pair_ceremony.log'
Remove-Item $script:pairLog -ErrorAction SilentlyContinue
$script:ceremonyCount = 0
$cp = $di.Pairing.Custom

$kinds = [Windows.Devices.Enumeration.DevicePairingKinds]::ConfirmOnly -bor `
         [Windows.Devices.Enumeration.DevicePairingKinds]::ConfirmPinMatch
Write-Host "kinds       : $kinds"

$ceremony = {
    param($sender, $args)
    try {
        [System.IO.File]::AppendAllText($script:pairLog, "FIRED kind=$($args.PairingKind)`r`n")
    } catch {}
    $script:ceremonyCount++
    try { $args.Accept() } catch {}
}

$evtInfo = $cp.GetType().GetEvent('PairingRequested')
if ($null -eq $evtInfo) { throw "PairingRequested event not found on $($cp.GetType().FullName)" }
$dt = $evtInfo.EventHandlerType
Write-Host "handler type: $($dt.FullName)"
$script:handler = [System.Management.Automation.LanguagePrimitives]::ConvertTo($ceremony, $dt)
[void]$cp.add_PairingRequested($script:handler)
Write-Host "handler attached via add_PairingRequested"

$rb = Await ($cp.PairAsync($kinds)) ([Windows.Devices.Enumeration.DevicePairingResult])
Write-Host "custom result: status=$($rb.Status) protection=$($rb.ProtectionLevelUsed) ceremonies=$script:ceremonyCount"
Write-Host "ceremony log : $((Get-Content $script:pairLog -ErrorAction SilentlyContinue) -join ' | ')"
Write-Host "paired(after): $($di.Pairing.IsPaired)"

if ($di.Pairing.IsPaired) { Write-Host 'PAIRED-OK(custom)'; exit 0 }
Write-Host 'PAIRED-FAIL'
exit 2
