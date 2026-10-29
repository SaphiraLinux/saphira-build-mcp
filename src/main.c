#include "core.h"
#include "mcp.h"
#include "tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *usage =
    "saphira-build-mcp " SBM_VERSION " - Saphira build/package queries\n"
    "and evidence journal\n"
    "\n"
    "usage:\n"
    "  saphira-build-mcp --mcp         MCP stdio server (23 built-in tools:\n"
    "                                  17 read-only, 4 journal writes\n"
    "                                  (notes, policy record, checkpoint\n"
    "                                  save/close), build request,\n"
    "                                  revision bump; plus tools.conf\n"
    "                                  externals when configured)\n"
    "  saphira-build-mcp --list-tools  print the tools/list JSON document\n"
    "  saphira-build-mcp --help        print this help and exit\n"
    "  saphira-build-mcp --version     print the version and exit\n"
    "\n"
    "Purpose: give AI agents a safe, structured window into a software\n"
    "build and package universe, plus a durable journal and checkpoint\n"
    "register so work survives across sessions. In portable mode\n"
    "(SBM_SAPHIRA=off) the same binary offers checkpoints, notes and\n"
    "externals on any host with no builder state.\n"
    "\n"
    "The server reads the Saphira builder state: recipes, the APK and\n"
    "repository databases, the build queue and retained logs. It never\n"
    "builds, signs, publishes, installs or restarts anything, and it never\n"
    "executes recipe scripts. Its only write surface is its own Egg-local\n"
    "evidence journal (package notes and policy records, checkpoints,\n"
    "and external-tool audit rows); backup of that journal is an\n"
    "operational concern (SQLite-safe snapshot via\n"
    "`sqlite3 notes.db .backup snapshot.db`, never a live file copy,\n"
    "never Git). Data-source paths default to the live system\n"
    "and can be pointed at fixtures for testing:\n"
    "  SBM_RECIPES SBM_QUEUE_DB SBM_REPO_DIR SBM_INCOMING SBM_TSV\n"
    "  SBM_PASSWD SBM_GROUP SBM_APKDB SBM_NOTES_DB SBM_TOOLS_CONF\n"
    "  SBM_BWRAP SBM_HOSTNAME SBM_USERNAME SBM_CWD\n"
    "Portable mode (SBM_SAPHIRA=off) gates every tool that needs builder\n"
    "state; checkpoints, the notes journal and configured externals keep\n"
    "working anywhere, with the journal at $SBM_NOTES_DB (recommended\n"
    "for portable hosts: ~/.local/share/saphira-build-mcp/notes.db).\n"
    "Unset or \"on\" keeps full builder behaviour; any other value is an\n"
    "explicit configuration failure.\n"
    "Journal attribution defaults to $SBM_AUTHOR (\"unspecified\" when unset);\n"
    "the MCP clientInfo recorder is client-reported and unverified.\n"
    "\n"
    "Saphira Linux - https://saphira.vm2.uk\n"
    "\n"
    "Created by AKADATA LIMITED for FREE.\n"
    "LICENSE applies for commercial use. See https://www.akadata.co.uk\n"
    "for more information.\n"
    "\n"
    "Upcoming product lines: Saphira-MemoryD and SHAMPOO -\n"
    "https://shampoo.op2.uk\n"
    "SHAMPOO stands for:\n"
    "Shared Human-Agent-Model Platform for Orchestration & Operations\n";

int main(int argc, char **argv) {
	const char *sm = getenv("SBM_SAPHIRA");
	/* --help / -h / --version are pure text: they never start the
	 * server, open a database, or emit protocol output, so they
	 * bypass configuration validation. Everything else validates
	 * first and fails explicitly. */
	if (argc == 2 && (strcmp(argv[1], "--help") == 0 ||
	    strcmp(argv[1], "-h") == 0)) {
		fputs(usage, stdout);
		return 0;
	}
	if (argc == 2 && strcmp(argv[1], "--version") == 0) {
		printf("%s %s\n", SBM_NAME, SBM_VERSION);
		return 0;
	}
	if (sm && strcmp(sm, "on") != 0 && strcmp(sm, "off") != 0) {
		fprintf(stderr, "saphira-build-mcp: invalid SBM_SAPHIRA='%s':"
		        " want 'on' or 'off' (or unset)\n", sm);
		return 2;
	}
	if (argc == 2 && strcmp(argv[1], "--mcp") == 0)
		return run_stdio();
	if (argc == 2 && strcmp(argv[1], "--list-tools") == 0) {
		gbuf b;
		gbuf_init(&b);
		tools_list_json(&b);
		if (!b.failed && b.p) {
			size_t n = strlen(b.p);
			if (fwrite(b.p, 1, n, stdout) != n) {
				gbuf_free(&b);
				return 1;
			}
			if (fputc('\n', stdout) == EOF) {
				gbuf_free(&b);
				return 1;
			}
			gbuf_free(&b);
			return 0;
		}
		gbuf_free(&b);
		return 1;
	}
	fputs(usage, stderr);
	return 2;
}
