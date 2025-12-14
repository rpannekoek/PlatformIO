$delayMs = 2000
$url = "http://xmas32.local/midi/start?delay=$delayMs"
$response = Invoke-WebRequest -Uri $url -UseBasicParsing
Write-Host $response.Content

$midiFilePath = Join-Path $PSScriptRoot "data\Let_It_Snow.mid"
& "C:\Program Files (x86)\Windows Media Player\wmplayer.exe" /Play "$midiFilePath"

Read-Host -Prompt "Press Enter to exit"
