if (-not (Test-Path 'NSISPlugins')) {
    New-Item -Name 'NSISPlugins' -ItemType 'Directory' | Out-Null
}
if (-not (Test-Path 'NSISPlugins\NScurl.zip')) {
    Write-Host 'Downloading NSCurl plugin...'
    Invoke-WebRequest 'https://github.com/negrutiu/nsis-nscurl/releases/download/v24.9.26.122/NScurl.zip' -OutFile 'NSISPlugins\NScurl.zip'
}
if (-not (Test-Path 'NSISPlugins\NScurl')) {
    Write-Host 'Extracting NSCurl plugin...'
    Expand-Archive -Path 'NSISPlugins\NScurl.zip' -DestinationPath 'NSISPlugins\NScurl'
}
