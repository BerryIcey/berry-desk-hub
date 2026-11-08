param([Parameter(Mandatory=$true)][string]$Library)
$ErrorActionPreference='Stop'
try {
    $expected='6EBC194316536BA61AF5BE24508AD9FCBB2ECC685E716C12E787C79530F66BF0'
    $hash=[System.Security.Cryptography.SHA256]::Create()
    try { $bytes=$hash.ComputeHash([System.IO.File]::ReadAllBytes($Library));$actual=([BitConverter]::ToString($bytes)).Replace('-','') } finally { $hash.Dispose() }
    if ($actual -ne $expected) { throw 'library_mismatch' }
    Add-Type -Path $Library
    $computer=New-Object LibreHardwareMonitor.Hardware.Computer
    $computer.IsCpuEnabled=$true
    $computer.Open()
    try {
        $values=@()
        foreach ($device in $computer.Hardware) {
            if ($device.HardwareType.ToString() -ne 'Cpu' -or $device.Name -notmatch 'AMD Ryzen') { continue }
            $device.Update()
            foreach ($sensor in $device.Sensors) {
                if ($sensor.SensorType.ToString() -eq 'Temperature' -and $sensor.Name -eq 'Core (Tctl/Tdie)' -and $null -ne $sensor.Value) {
                    $value=[double]$sensor.Value
                    if ($value -ge 0 -and $value -le 120) { $values+= $value }
                }
            }
        }
        if ($values.Count -eq 1) { @{temperature_c=$values[0]} | ConvertTo-Json -Compress }
        else { '{"temperature_c":null}' }
    } finally { $computer.Close() }
} catch { '{"temperature_c":null}' }
