<#
.SYNOPSIS
  Minimal standalone MCP stdio client for saphira-build-mcp (PowerShell).

.DESCRIPTION
  Same NDJSON framing as docs/examples/mcp-stdio-client.py: launches the
  server with --mcp, sends initialize + notifications/initialized, then
  tools/list or tools/call, one JSON object per line, UTF-8 without BOM.

  Native Windows example (no Wine needed for the client itself):

    .\mcp-stdio-client.ps1 -Exe .\saphira-build-mcp.windows-builtin.exe -Cmd list
    .\mcp-stdio-client.ps1 -Exe .\saphira-build-mcp.windows-builtin.exe -Cmd call `
      -Name checkpoint_list -Arguments '{"limit": 2}'

  Server environment (SBM_NOTES_DB, SBM_SAPHIRA, SBM_BUILD,
  SBM_TOOLS_CONF) is inherited from this shell, exactly as OpenCode or
  Codex would set it per MCP server entry.

.NOTES
  Review-validated against the Python client and the v0.10.8 framing
  code (src/mcp.c). Not executed in Linux CI (no PowerShell runtime);
  the framing rules it implements are byte-identical to the verified
  Python client.
#>
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][ValidateSet("init", "list", "call")][string]$Cmd,
    [string]$Name = "",
    [string]$Arguments = "{}"
)

$ErrorActionPreference = "Stop"
$nextId = 1

$p = New-Object System.Diagnostics.Process
$p.StartInfo.FileName = $Exe
$p.StartInfo.Arguments = "--mcp"
$p.StartInfo.RedirectStandardInput = $true
$p.StartInfo.RedirectStandardOutput = $true
$p.StartInfo.RedirectStandardError = "nul"
$p.StartInfo.UseShellExecute = $false
$p.StartInfo.StandardInputEncoding = New-Object System.Text.UTF8Encoding($false)
$p.StartInfo.StandardOutputEncoding = New-Object System.Text.UTF8Encoding($false)
[void]$p.Start()
$stdin = $p.StandardInput
$stdout = $p.StandardOutput

function Send-Request([string]$Method, $Params) {
    $msg = @{ jsonrpc = "2.0"; id = $script:nextId; method = $Method }
    if ($null -ne $Params) { $msg["params"] = $Params }
    $script:nextId++
    $stdin.WriteLine(($msg | ConvertTo-Json -Depth 12 -Compress))
    $stdin.Flush()
    $line = $stdout.ReadLine()
    if (-not $line) { throw "server closed stdout (EOF)" }
    return ($line | ConvertFrom-Json)
}

function Send-Notify([string]$Method) {
    $stdin.WriteLine((@{ jsonrpc = "2.0"; method = $Method } | ConvertTo-Json -Compress))
    $stdin.Flush()
}

try {
    $init = Send-Request "initialize" @{
        protocolVersion = "2025-06-18"
        clientInfo      = @{ name = "mcp-stdio-client-ps1"; version = "1.0" }
    }
    Send-Notify "notifications/initialized"
    $init.result | ConvertTo-Json -Depth 8
    if ($Cmd -in @("list", "call")) {
        $list = Send-Request "tools/list" $null
        if ($Cmd -eq "list") {
            $list.result.tools | ForEach-Object { $_.name }
        } else {
            $res = Send-Request "tools/call" @{
                name      = $Name
                arguments = ($Arguments | ConvertFrom-Json)
            }
            if ($res.error) { $res.error | ConvertTo-Json -Depth 6; exit 3 }
            $res.result | ConvertTo-Json -Depth 8
        }
    }
} finally {
    $stdin.Close()
    $p.WaitForExit(10000) | Out-Null
}
