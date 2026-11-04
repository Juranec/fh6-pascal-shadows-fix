# Reads the export table of the real System32\d3d12.dll (needs dumpbin on PATH: run from a Visual
# Studio developer prompt) and generates, preserving the ORIGINAL ORDINALS (the game imports by ordinal):
#   fwd.asm        jump stubs + g_fwd pointer table
#   fwd_names.inc  export names / ordinals (same order as the table)
#   exports.rsp    linker /EXPORT options
# D3D12CreateDevice is implemented in d3d12_proxy.cpp.
$ErrorActionPreference = 'Stop'
$real = Join-Path $env:SystemRoot 'System32\d3d12.dll'
$lines = & dumpbin /exports $real
$items = New-Object System.Collections.Generic.List[object]
$createOrd = $null
foreach ($l in $lines) {
    if ($l -match '^\s+(\d+)\s+(?:[0-9A-Fa-f]+\s+)?[0-9A-Fa-f]{8}\s+(\S+)') {
        $ord = [int]$Matches[1]; $name = $Matches[2]
        if ($name -eq 'D3D12CreateDevice') { $createOrd = $ord; continue }
        if ($name -eq '[NONAME]') {
            $items.Add([pscustomobject]@{ Stub = "fwd_ord$ord"; Ord = $ord; Orig = $null })
        } else {
            $items.Add([pscustomobject]@{ Stub = $name; Ord = $ord; Orig = $name })
        }
    }
}
if ($items.Count -eq 0) { throw 'No exports parsed from dumpbin output' }
if ($createOrd -eq $null) { throw 'D3D12CreateDevice not found in dumpbin output' }

$asm = New-Object System.Collections.Generic.List[string]
$asm.Add('.data')
$asm.Add('PUBLIC g_fwd')
$asm.Add("g_fwd QWORD $($items.Count) DUP (0)")
$asm.Add('.code')
$i = 0
foreach ($it in $items) {
    $asm.Add("$($it.Stub) PROC")
    $asm.Add("    jmp qword ptr [g_fwd + $($i * 8)]")
    $asm.Add("$($it.Stub) ENDP")
    $i++
}
$asm.Add('END')
$asm | Set-Content -Encoding ascii fwd.asm

$inc = New-Object System.Collections.Generic.List[string]
$inc.Add('static const char* const kFwdNames[] = {')
foreach ($it in $items) { if ($it.Orig) { $inc.Add('    "' + $it.Orig + '",') } else { $inc.Add('    nullptr,') } }
$inc.Add('};')
$inc.Add('static const unsigned short kFwdOrds[] = {')
foreach ($it in $items) { $inc.Add("    $($it.Ord),") }
$inc.Add('};')
$inc | Set-Content -Encoding ascii fwd_names.inc

$rsp = New-Object System.Collections.Generic.List[string]
$rsp.Add("/EXPORT:D3D12CreateDevice=Proxy_D3D12CreateDevice,@$createOrd")
foreach ($it in $items) {
    if ($it.Orig) { $rsp.Add("/EXPORT:$($it.Stub),@$($it.Ord)") }
    else { $rsp.Add("/EXPORT:$($it.Stub),@$($it.Ord),NONAME") }
}
$rsp | Set-Content -Encoding ascii exports.rsp

Write-Host "generated stubs for $($items.Count) exports (D3D12CreateDevice = ordinal $createOrd)"
