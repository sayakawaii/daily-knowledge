# 注册/更新「每日一课」提醒的 Windows 计划任务：工作日 10:00 运行 remind.ps1。
# 用法（Windows PowerShell，不需要管理员权限）：
#   powershell -ExecutionPolicy Bypass -File scripts\install-reminder.ps1
# 卸载：
#   Unregister-ScheduledTask -TaskName 'Daily Knowledge Reminder' -Confirm:$false

$ErrorActionPreference = 'Stop'

$script = Join-Path $PSScriptRoot 'remind.ps1'
if (-not (Test-Path $script)) {
    throw "找不到 remind.ps1：$script"
}

$action = New-ScheduledTaskAction -Execute 'powershell.exe' `
    -Argument ('-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File "{0}"' -f $script)

$trigger = New-ScheduledTaskTrigger -Weekly `
    -DaysOfWeek Monday, Tuesday, Wednesday, Thursday, Friday -At '10:00'

# StartWhenAvailable：如果 10 点时电脑是关的/睡着的，开机后补跑一次。
$settings = New-ScheduledTaskSettingsSet -StartWhenAvailable `
    -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit (New-TimeSpan -Minutes 5)

Register-ScheduledTask -TaskName 'Daily Knowledge Reminder' `
    -Description '工作日 10:00 提醒上每日一课，并把 Cursor 打开到 daily-knowledge 工作区' `
    -Action $action -Trigger $trigger -Settings $settings -Force | Out-Null

Write-Output '已注册计划任务：Daily Knowledge Reminder（工作日 10:00）'
