# 每日一课提醒：弹一条 Windows 通知，并把 Cursor 打开到这个工作区。
# 由计划任务「每日一课提醒」在工作日 10:00 调用（需用 Windows PowerShell 5.1 运行）。

$ErrorActionPreference = 'SilentlyContinue'

$workspace = Join-Path $HOME 'daily-knowledge'
$cursor = Join-Path $env:LOCALAPPDATA 'Programs\cursor\Cursor.exe'

try {
    [void][Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType = WindowsRuntime]
    [void][Windows.Data.Xml.Dom.XmlDocument, Windows.Data.Xml.Dom, ContentType = WindowsRuntime]

    $xml = @"
<toast>
  <visual>
    <binding template="ToastText02">
      <text id="1">每日一课</text>
      <text id="2">今天这一课该上了。在 Cursor 里说一声「开始」，挑个分类。</text>
    </binding>
  </visual>
</toast>
"@

    $doc = New-Object Windows.Data.Xml.Dom.XmlDocument
    $doc.LoadXml($xml)
    $toast = New-Object Windows.UI.Notifications.ToastNotification $doc
    $appId = '{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\WindowsPowerShell\v1.0\powershell.exe'
    [Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier($appId).Show($toast)
} catch {
    # 通知失败不影响后面打开工作区
}

if (Test-Path $cursor) {
    Start-Process -FilePath $cursor -ArgumentList $workspace
}
