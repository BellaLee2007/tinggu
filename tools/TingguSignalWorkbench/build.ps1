$ErrorActionPreference = 'Stop'
$compiler = 'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$source = Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.cs' | ForEach-Object { $_.FullName }

& $compiler /nologo /optimize+ /target:winexe /out:"$PSScriptRoot\TingguSignalWorkbench.exe" `
  /reference:System.dll `
  /reference:System.Core.dll `
  /reference:System.Drawing.dll `
  /reference:System.Windows.Forms.dll `
  /reference:System.Windows.Forms.DataVisualization.dll `
  $source

if ($LASTEXITCODE -ne 0) { throw "C# build failed with exit code $LASTEXITCODE" }
Write-Host 'Built TingguSignalWorkbench.exe'
