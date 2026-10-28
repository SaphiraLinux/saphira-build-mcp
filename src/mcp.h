#ifndef SBM_MCP_H
#define SBM_MCP_H

#define SBM_NAME "saphira-build-mcp"
#define SBM_VERSION "0.1.0"

#define SBM_MAX_REQUEST_BYTES (64u * 1024u)
#define SBM_MAX_HEADER_BYTES 16384u
#define SBM_MAX_QUERY_BYTES 8192u
#define SBM_MAX_RESULT_BYTES (256u * 1024u)
#define SBM_MAX_OUTPUT_FILE_BYTES (2u * 1024u * 1024u)

/* Static tools/list result: descriptions and schemas stay minimal.
 * Eleven read-only tools. No submit, bump, sign, publish, install
 * or restart surface exists by design, not by omission. */
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
	"{\"name\":\"package_identity\",\"description\":\"Unix identity: fragment, registry, reservations, live accounts.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}}" \
	"]}"

int run_stdio(void);

#endif
