$ErrorActionPreference='Continue'
$base = $PSScriptRoot
Write-Host "DroidVM ARM64 driver installer" -ForegroundColor Cyan
# NetKVM LAST: on a protected VM, touching a live NIC can bugcheck; do the others first.
$order = @('rdmapool','viostor','vioinput','NetKVM')
function Get-Pkgs {
  $r=@(); $pub=$null
  foreach($l in (pnputil /enum-drivers)){
    if($l -match 'Published Name\s*:\s*(\S+)'){ $pub=$matches[1] }
    elseif(($l -match 'Original Name\s*:\s*(\S+)') -and $pub){ $r += [pscustomobject]@{Pub=$pub;Orig=$matches[1].ToLower()}; $pub=$null }
  }
  ,$r
}
foreach($d in $order){
  $inf = Get-ChildItem (Join-Path $base "drivers\$d") -Filter *.inf -ErrorAction SilentlyContinue | Select-Object -First 1
  if(-not $inf){ Write-Host "skip $d (no inf found)" -ForegroundColor Yellow; continue }
  $orig = $inf.Name.ToLower()
  Write-Host ""
  Write-Host ("== install " + $d + " : " + $inf.Name + " ==") -ForegroundColor Cyan
  $before = @(Get-Pkgs | Where-Object { $_.Orig -eq $orig } | ForEach-Object Pub)
  pnputil /add-driver $inf.FullName /install | Write-Host
  $after = @(Get-Pkgs | Where-Object { $_.Orig -eq $orig } | ForEach-Object Pub)
  $new = @($after | Where-Object { $_ -notin $before }) | Select-Object -First 1
  if(-not $new){ $new = $after | Select-Object -Last 1 }
  foreach($p in $after){ if($p -ne $new){ Write-Host ("  remove old " + $p); pnputil /delete-driver $p /uninstall | Out-Null } }
}
Write-Host ""
Write-Host "DONE. Reboot so the boot drivers (viostor/rdmapool) load the new version." -ForegroundColor Green
