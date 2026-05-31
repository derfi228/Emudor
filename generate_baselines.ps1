# Baseline screenshots generator for Emudor
# Usage: .\generate_baselines.ps1 [-Console all|nes|snes]

param(
    [string]$Console = "all"
)

$EmulatorExe = ".\build\debug\emudor.exe"
$Frames = 1800
$ScreenshotEvery = 300

function Process-Console {
    param($consoleName, $consoleNameUpper, $extensions)
    
    $romsDir = "roms\$consoleName"
    $baselineDir = "agent_data\baselines\$consoleName"
    
    if (-not (Test-Path $romsDir)) {
        Write-Host "Folder $romsDir not found, skipping" -ForegroundColor Yellow
        return
    }
    
    Write-Host ""
    Write-Host "=== Processing $consoleNameUpper ===" -ForegroundColor Cyan
    
    foreach ($ext in $extensions) {
        $roms = Get-ChildItem -Path $romsDir -Filter "*$ext" -File
        
        foreach ($rom in $roms) {
            $gameName = [System.IO.Path]::GetFileNameWithoutExtension($rom.Name)
            $safeName = $gameName -replace '[^a-zA-Z0-9_-]', '_'
            $gameBaselineDir = "$baselineDir\$safeName"
            
            if (Test-Path $gameBaselineDir) {
                Write-Host "[SKIP] $gameName - baseline exists" -ForegroundColor DarkGray
                continue
            }
            
            New-Item -ItemType Directory -Path $gameBaselineDir -Force | Out-Null
            
            $outputPattern = "$gameBaselineDir\frame.png"
            
            Write-Host "[RUN]  $gameName" -ForegroundColor Green
            
            $proc = Start-Process -FilePath $EmulatorExe `
                -ArgumentList "--rom `"$($rom.FullName)`"", `
                              "--console $consoleNameUpper", `
                              "--frames $Frames", `
                              "--screenshot-every $ScreenshotEvery", `
                              "--screenshot `"$outputPattern`"", `
                              "--headless" `
                -Wait -PassThru -NoNewWindow
            
            if ($proc.ExitCode -ne 0) {
                Write-Host "       FAIL: exit code $($proc.ExitCode)" -ForegroundColor Red
                Remove-Item -Recurse -Force $gameBaselineDir
            } else {
                $screenshotCount = (Get-ChildItem $gameBaselineDir -Filter "*.png").Count
                Write-Host "       OK: $screenshotCount screenshots" -ForegroundColor Green
            }
        }
    }
}

if ($Console -eq "all" -or $Console -eq "nes") {
    Process-Console -consoleName "nes" -consoleNameUpper "NES" -extensions @(".nes")
}

if ($Console -eq "all" -or $Console -eq "snes") {
    Process-Console -consoleName "snes" -consoleNameUpper "SNES" -extensions @(".sfc", ".smc")
}

Write-Host ""
Write-Host "Done! Check baselines in agent_data\baselines\" -ForegroundColor Cyan