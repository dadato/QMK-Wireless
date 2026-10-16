# 列出系统已知的 BLE 设备（未配对 + 已配对），确认键盘模组是否可见。
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Runtime.WindowsRuntime

[void][Windows.Devices.Enumeration.DeviceInformation,Windows.Devices.Enumeration,ContentType=WindowsRuntime]
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

foreach ($paired in @($false, $true)) {
    $aqs = [Windows.Devices.Bluetooth.BluetoothLEDevice]::GetDeviceSelectorFromPairingState($paired)
    $devs = Await ([Windows.Devices.Enumeration.DeviceInformation]::FindAllAsync($aqs)) ([Windows.Devices.Enumeration.DeviceInformationCollection])
    "---- paired=$paired  count=$($devs.Count) ----"
    foreach ($d in $devs) {
        "{0,-34} {1}" -f $d.Name, $d.Id
    }
}
