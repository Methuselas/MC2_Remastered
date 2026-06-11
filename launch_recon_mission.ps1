# Launch mission with GOM_RECON instrumentation enabled
# Usage: .\launch_recon_mission.ps1 -Mission mc2_01 -Duration 30

param(
    [string]$Mission = "mc2_01",
    [int]$Duration = 30
)

# Set environment variable for GOM recon instrumentation
$env:MC2_GOM_RECON = "1"

Write-Host "Launching $Mission with MC2_GOM_RECON=1 for $Duration seconds..."
Write-Host "Game executable: A:\Games\mc2-opengl\mc2-win64-v0.4\mc2.exe"
Write-Host "Mission: $Mission"
Write-Host ""

# Launch game with mission and duration
& "A:\Games\mc2-opengl\mc2-win64-v0.4\mc2.exe" -mission $Mission -duration $Duration

Write-Host "Mission completed."
