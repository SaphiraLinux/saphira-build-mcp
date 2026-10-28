#include "core.h"
#include "mcp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *usage =
    "saphira-build-mcp - Saphira build/package queries and evidence journal\n"
    "\n"
    "usage:\n"
    "  saphira-build-mcp --mcp         MCP stdio server (16 tools: 14 read-only,\n"
    "                                  2 journal-scoped writes to notes.db only)\n"
    "  saphira-build-mcp --list-tools  print the tools/list JSON document\n"
    "\n"
    "The server reads the Saphira builder state: recipes, the APK and\n"
    "repository databases, the build queue and retained logs. It never\n"
    "builds, signs, publishes, installs or restarts anything, and it never\n"
    "executes recipe scripts. Its only write surface is its own Egg-local\n"
    "evidence journal (package notes and policy records); backup of that\n"
    "journal is an operational concern (SQLite-safe snapshot via\n"
    "`sqlite3 notes.db .backup snapshot.db`, never a live file copy,\n"
    "never Git). Data-source paths default to the live system\n"
    "and can be pointed at fixtures for testing:\n"
    "  SBM_RECIPES SBM_QUEUE_DB SBM_REPO_DIR SBM_INCOMING SBM_TSV\n"
    "  SBM_PASSWD SBM_GROUP SBM_APKDB SBM_NOTES_DB\n"
    "Journal attribution defaults to $SBM_AUTHOR (\"unspecified\" when unset);\n"
    "the MCP clientInfo recorder is client-reported and unverified.\n";

int main(int argc, char **argv) {
	if (argc == 2 && strcmp(argv[1], "--mcp") == 0)
		return run_stdio();
	if (argc == 2 && strcmp(argv[1], "--list-tools") == 0) {
		size_t n = strlen(SBM_TOOLS_LIST_JSON);
		if (fwrite(SBM_TOOLS_LIST_JSON, 1, n, stdout) != n)
			return 1;
		if (fputc('\n', stdout) == EOF)
			return 1;
		return 0;
	}
	fputs(usage, stderr);
	return 2;
}
