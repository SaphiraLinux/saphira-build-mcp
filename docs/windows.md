# Windows: builds, Wine testing, OpenCode config (v0.10.8)

## Cross-build (MinGW, on Linux)

Two targets, both proven with MinGW-GCC 16.2.0:

```sh
make windows           # shared sqlite: needs sqlite3.dll beside the exe
make windows-builtin   # self-contained: sqlite amalgamation 3.53.x compiled in
```

| Artifact | Size | Needs |
|---|---|---|
| `saphira-build-mcp.windows.exe` | ~117 KiB | `sqlite3.dll` (~1.6 MiB, FTS5-capable, same directory) |
| `saphira-build-mcp.windows-builtin.exe` | ~875 KiB | nothing |
| `sqlite3.dll` | ~1.6 MiB | beside the shared exe |

(For scale: the musl static builtin is ~818 KiB vs ~99 KiB shared —
the builtin premium is the database engine itself, on every platform.)

Portability notes from the implementation (`src/wincompat.h`, `src/mcp.c`):

- stdio is forced `_O_BINARY`: without it the CRT translates `\n`
  to `\r\n`, corrupting NDJSON framing. stderr stays text mode.
- A single trailing `\r` is stripped as framing on input; output is
  LF-only — so Unix clients, pipes and PowerShell all interoperate.
- Journal paths tolerate drive letters and backslashes; the tool
  refuses to escape its configured directories (`..` rejected).
- No `fork`, no `execvp` search, no signals: process and timeout
  handling use the native equivalents behind the same interface.

## Wine testing (Wine 11.20, no Windows licence needed)

The committed `.exe` files run under Wine on Linux. Capability matrix
on the v0.10.8 `windows.exe` (identical binary bytes as shipped):

```sh
export WINEDEBUG=-all
wine saphira-build-mcp.windows.exe --version
# saphira-build-mcp 0.10.8
wine saphira-build-mcp.windows.exe --list-tools | grep -o '"name":"' | wc -l
# 23
SBM_BUILD=off wine saphira-build-mcp.windows.exe --list-tools | grep -o '"name":"' | wc -l
# 21
SBM_SAPHIRA=off wine saphira-build-mcp.windows.exe --list-tools | grep -o '"name":"' | wc -l
# 12
SBM_SAPHIRA=off SBM_BUILD=off wine saphira-build-mcp.windows.exe --list-tools | grep -o '"name":"' | wc -l
# 10
```

Same 23/21/12/10 as every Unix build. MCP sessions work over Wine
pipes (`printf ... | wine ... --mcp`), including `Content-Length`
framing and CRLF input.

## OpenCode configuration with the Windows exe

Proven live on Arch Linux (Homer): the Windows **builtin** exe serves
as a real OpenCode MCP server through Wine, against the same journal
file as the native entry. `~/.config/opencode/opencode.jsonc`:

```json
"saphira-build-windows": {
  "type": "local",
  "command": [
    "wine",
    "/home/smalley/.local/bin/saphira-build-mcp.windows-builtin.exe",
    "--mcp"
  ],
  "environment": {
    "SBM_SAPHIRA": "off",
    "SBM_NOTES_DB": "/home/smalley/.local/share/saphira-build-mcp/notes.db",
    "SBM_AUTHOR": "homer-win",
    "WINEDEBUG": "-all"
  },
  "enabled": true
}
```

- Builtin, not shared: one file, no DLL search path under Wine.
- Same `SBM_NOTES_DB` as the native server: one journal, two
  runtimes. `SBM_AUTHOR` differs (`homer-win`) so provenance tells
  which runtime wrote each row.
- `opencode mcp list` shows `saphira-build-windows ✓ connected`
  (that badge already means initialize + tools/list succeeded), and an
  agent `total_recall` through it returned live journal rows.

## Native Windows usage (PowerShell)

No Wine, no WSL — the exe is native:

```powershell
$env:SBM_NOTES_DB = "$env:LOCALAPPDATA\saphira-build-mcp\notes.db"
$env:SBM_TOOLS_CONF = "C:\saphira\mcp\tools.conf"
.\saphira-build-mcp.windows-builtin.exe --version
.\mcp-stdio-client.ps1 -Exe .\saphira-build-mcp.windows-builtin.exe -Cmd list
```

Journal and config paths are ordinary Windows paths. Everything else
— framing, capabilities, externals grammar — is identical: the `.c`
sources are shared, only the toolchain differs.
