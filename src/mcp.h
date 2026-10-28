#ifndef SBM_MCP_H
#define SBM_MCP_H

#define SBM_NAME "saphira-build-mcp"
#define SBM_VERSION "0.5.0"

#define SBM_MAX_REQUEST_BYTES (64u * 1024u)
#define SBM_MAX_HEADER_BYTES 16384u
#define SBM_MAX_QUERY_BYTES 8192u
#define SBM_MAX_RESULT_BYTES (256u * 1024u)
#define SBM_MAX_OUTPUT_FILE_BYTES (2u * 1024u * 1024u)

/* Static tools/list result: descriptions and schemas stay minimal.
 * Fifteen read-only tools plus three journal-scoped writes (notes.db
 * only - no recipe, queue, revision, signing, publish, install or
 * restart surface exists by design, not by omission), plus two
 * daemon/operator ops (build_request queues detached work, package_bump
 * applies bumppkg guards; neither signs, publishes nor installs). */
#define SBM_TOOLS_LIST_JSON \
	"{\"tools\":[" \
	"{\"name\":\"package_search\",\"description\":\"Find packages by name or description substring.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"limit\":{\"type\":\"number\"},\"include_disabled\":{\"type\":\"boolean\"}},\"required\":[\"query\"]}}," \
	"{\"name\":\"package_info\",\"description\":\"Recipe fields and repository newest for a package.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}}," \
	"{\"name\":\"package_dependencies\",\"description\":\"Dependency edges for a package.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"direction\":{\"type\":\"string\",\"enum\":[\"deps\",\"reverse\",\"both\"]}},\"required\":[\"name\"]}}," \
	"{\"name\":\"package_why_installed\",\"description\":\"Explain dependency chain(s) from a package to a dependency.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"package\":{\"type\":\"string\"},\"dep\":{\"type\":\"string\"}},\"required\":[\"package\",\"dep\"]}}," \
	"{\"name\":\"package_installed\",\"description\":\"Installed version versus repository newest.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}}," \
	"{\"name\":\"package_build_status\",\"description\":\"Latest build verdict, timings and detail.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"include_disabled\":{\"type\":\"boolean\"}},\"required\":[\"name\"]}}," \
	"{\"name\":\"package_build_history\",\"description\":\"Previous build attempts with timings and SBU.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"limit\":{\"type\":\"number\"},\"include_disabled\":{\"type\":\"boolean\"}},\"required\":[\"name\"]}}," \
	"{\"name\":\"package_build_log\",\"description\":\"Retained build/install log tail.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"stage\":{\"type\":\"string\",\"enum\":[\"build\",\"install\",\"both\"]},\"tail_bytes\":{\"type\":\"number\"}},\"required\":[\"name\"]}}," \
	"{\"name\":\"package_artifacts\",\"description\":\"Staged, signed and published artifacts.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}}," \
	"{\"name\":\"build_queue\",\"description\":\"Queued and active builds.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"limit\":{\"type\":\"number\"}}}}," \
	"{\"name\":\"package_identity\",\"description\":\"Unix identity: fragment, registry, reservations, live accounts.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}}," \
 "{\"name\":\"package_notes_get\",\"description\":\"Read engineering notes for a package (newest first).\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"package\":{\"type\":\"string\"},\"kind\":{\"type\":\"string\",\"enum\":[\"finding\",\"decision\",\"issue\",\"success\",\"failure\",\"lesson\"]},\"limit\":{\"type\":\"number\"}},\"required\":[\"package\"]}}," \
 "{\"name\":\"package_notes_add\",\"description\":\"Record an engineering finding, decision, open issue, success, failure or lesson (journal only).\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"package\":{\"type\":\"string\"},\"kind\":{\"type\":\"string\",\"enum\":[\"finding\",\"decision\",\"issue\",\"success\",\"failure\",\"lesson\"]},\"title\":{\"type\":\"string\"},\"body\":{\"type\":\"string\"},\"author\":{\"type\":\"string\"}},\"required\":[\"package\",\"kind\",\"body\"]}}," \
	"{\"name\":\"package_policy_status\",\"description\":\"Policy checks with evidence and freshness for a package.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"package\":{\"type\":\"string\"}},\"required\":[\"package\"]}}," \
	"{\"name\":\"package_policy_record\",\"description\":\"Attach verified evidence to a policy check (journal only).\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"package\":{\"type\":\"string\"},\"check\":{\"type\":\"string\",\"enum\":[\"unix-identity\",\"payload-layout\",\"revision-discipline\",\"dependency-closure\",\"signing-acceptance\"]},\"result\":{\"type\":\"string\",\"enum\":[\"PASS\",\"FAIL\"]},\"evidence\":{\"type\":\"object\"},\"author\":{\"type\":\"string\"}},\"required\":[\"package\",\"check\",\"result\"]}}," \
 "{\"name\":\"package_readiness\",\"description\":\"What is verified, stale, unverified, plus open issues and recent notes.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"package\":{\"type\":\"string\"}},\"required\":[\"package\"]}}," \
	"{\"name\":\"build_request\",\"description\":\"Queue a build through the daemon intake (scan); the daemon executes detached. Records the reason in the journal.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"package\":{\"type\":\"string\"},\"reason\":{\"type\":\"string\"},\"author\":{\"type\":\"string\"}},\"required\":[\"package\",\"reason\"]}}," \
	"{\"name\":\"package_bump\",\"description\":\"Revision bump with bumppkg guards; the reason is journaled. Never signs, publishes or installs.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"package\":{\"type\":\"string\"},\"reason\":{\"type\":\"string\"},\"author\":{\"type\":\"string\"}},\"required\":[\"package\",\"reason\"]}}," \
	"{\"name\":\"checkpoint_save\",\"description\":\"Save a pre-restart session checkpoint (journal only). Resume post-restart with checkpoint_resume.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"label\":{\"type\":\"string\"},\"title\":{\"type\":\"string\"},\"body\":{\"type\":\"string\"},\"package\":{\"type\":\"string\"},\"src_commit\":{\"type\":\"string\"},\"recipe_commit\":{\"type\":\"string\"},\"author\":{\"type\":\"string\"}},\"required\":[\"label\",\"body\"]}}," \
	"{\"name\":\"checkpoint_resume\",\"description\":\"Resume a saved checkpoint by id, by label (newest), or newest overall when neither is given.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"number\"},\"label\":{\"type\":\"string\"}}}}" \
	"]}"

int run_stdio(void);

#endif
