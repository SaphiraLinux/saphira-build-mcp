#include "core.h"
#include "mcp.h"
#include "tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *usage =
    "saphira-build-mcp - Saphira build/package queries and evidence journal\n"
    "\n"
    "usage:\n"
    "  saphira-build-mcp --mcp         MCP stdio server (22 built-in tools:\n"
    "                                  16 read-only, 4 journal writes\n"
    "                                  (notes, policy record, checkpoint\n"
    "                                  save/close), build request,\n"
    "                                  revision bump; plus tools.conf\n"
    "                                  externals when configured)\n"
    "  saphira-build-mcp --list-tools  print the tools/list JSON document\n"
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
    "  SBM_BWRAP\n"
    "Portable mode (SBM_SAPHIRA=off) gates every tool that needs builder\n"
    "state; checkpoints, the notes journal and configured externals keep\n"
    "working anywhere, with the journal at $SBM_NOTES_DB (recommended\n"
    "for portable hosts: ~/.local/share/saphira-build-mcp/notes.db).\n"
    "Unset or \"on\" keeps full builder behaviour; any other value is an\n"
    "explicit configuration failure.\n"
    "Journal attribution defaults to $SBM_AUTHOR (\"unspecified\" when unset);\n"
    "the MCP clientInfo recorder is client-reported and unverified.\n";

int main(int argc, char **argv) {
	const char *sm = getenv("SBM_SAPHIRA");
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
