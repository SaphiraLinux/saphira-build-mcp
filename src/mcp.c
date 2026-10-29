#include "mcp.h"
#include "json.h"
#include "tools.h"
#include "wincompat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>

/* ASCII case-insensitive compare for protocol keywords (header
 * names are ASCII by construction). Replaces strncasecmp so no
 * <strings.h> dependency exists anywhere in the tree. */
static int ascii_ncasecmp(const char *a, const char *b, size_t n) {
	for (; n; n--, a++, b++) {
		unsigned char ca = (unsigned char)*a;
		unsigned char cb = (unsigned char)*b;
		if (ca >= 'A' && ca <= 'Z')
			ca += (unsigned char)('a' - 'A');
		if (cb >= 'A' && cb <= 'Z')
			cb += (unsigned char)('a' - 'A');
		if (ca != cb)
			return (int)ca - (int)cb;
		if (!ca)
			break;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* JSON-RPC helpers                                                    */
/* ------------------------------------------------------------------ */
static void jsonrpc_result(gbuf *out, const char *idraw, size_t idlen, const char *result_json) {
	gbuf_adds(out, "{\"jsonrpc\":\"2.0\",\"id\":");
	gbuf_addn(out, idraw, idlen);
	gbuf_adds(out, ",\"result\":");
	gbuf_adds(out, result_json);
	gbuf_adds(out, "}");
}

static void jsonrpc_error(gbuf *out, const char *idraw, size_t idlen, int code, const char *msg) {
	char tmp[32];
	gbuf_adds(out, "{\"jsonrpc\":\"2.0\",\"id\":");
	gbuf_addn(out, idraw, idlen);
	gbuf_adds(out, ",\"error\":{\"code\":");
	snprintf(tmp, sizeof tmp, "%d", code);
	gbuf_adds(out, tmp);
	gbuf_adds(out, ",\"message\":");
	json_escape(out, msg, strlen(msg));
	gbuf_adds(out, "}}");
}

static void bad_id(gbuf *out, int code, const char *msg) {
	jsonrpc_error(out, "null", 4, code, msg);
}

/* parse one MCP JSON-RPC message; writes a response into resp if one is due */
static int handle_mcp(const char *body, size_t len, gbuf *resp) {
	const char *noraw = "null";
	if (len > SBM_MAX_REQUEST_BYTES) {
		bad_id(resp, -32600, "request too large");
		return 1;
	}
	jv *root = json_parse(body, len);
	if (!root) {
		bad_id(resp, -32700, "parse error");
		return 1;
	}
	jv *id = jv_get(root, "id");
	jv *method = jv_get(root, "method");
	const char *idraw = (id && id->raw) ? id->raw : noraw;
	size_t idlen = (id && id->raw) ? id->rawlen : 4;
	if (!method || method->t != 3 || !method->str) {
		jsonrpc_error(resp, idraw, idlen, -32600,
		              !method ? "invalid request" : "method must be a string");
		jv_free(root);
		return 1;
	}
	const char *m = method->str;

	if (strcmp(m, "notifications/initialized") == 0) {
		jv_free(root);
		return 0; /* notification: no response */
	}
	if (!id || !id->raw) {
		jv_free(root);
		return 0; /* other notifications: silent */
	}

	if (strcmp(m, "initialize") == 0) {
		const char *pv = "2025-03-26";
		jv *params = jv_get(root, "params");
		if (params) {
			jv *p = jv_get(params, "protocolVersion");
			jv *ci = jv_get(params, "clientInfo");
			if (p && p->str)
				pv = p->str;
			/* Recorder identity for journal attribution:
			 * clientInfo is client-reported metadata, kept
			 * verbatim and never trusted for decisions. */
			if (ci && ci->t == 5) {
				jv *cn = jv_get(ci, "name");
				jv *cv = jv_get(ci, "version");
				char rec[128];
				if (cn && cn->str && cn->str[0] && cv && cv->str && cv->str[0])
					snprintf(rec, sizeof rec, "%s/%s", cn->str, cv->str);
				else if (cn && cn->str && cn->str[0])
					snprintf(rec, sizeof rec, "%s", cn->str);
				else
					rec[0] = 0;
				sbm_set_recorder(rec);
			} else {
				sbm_set_recorder("");
			}
		} else {
			sbm_set_recorder("");
		}
		gbuf r, ins;
		gbuf_init(&r);
		gbuf_init(&ins);
		/* The checkpoint register at connect time: static
		 * guidance plus the live active set. Read-only and
		 * fail-open (an unavailable journal yields the static
		 * half); a new agent learns the register without
		 * guessing ids. */
		checkpoint_instructions(&ins);
		gbuf_adds(&r, "{\"protocolVersion\":");
		json_escape(&r, pv, strlen(pv));
		gbuf_adds(&r,
		          ",\"capabilities\":{\"tools\":{\"listChanged\":false}},"
		          "\"serverInfo\":{\"name\":\"" SBM_NAME "\",\"version\":\"" SBM_VERSION "\"},"
		          "\"instructions\":");
		if (ins.failed || !ins.p)
			gbuf_adds(&r, "\"\"");
		else
			json_escape(&r, ins.p, ins.len);
		gbuf_adds(&r, "}");
		jsonrpc_result(resp, idraw, idlen, r.p ? r.p : "{}");
		gbuf_free(&r);
		gbuf_free(&ins);
	} else if (strcmp(m, "ping") == 0) {
		jsonrpc_result(resp, idraw, idlen, "{}");
	} else if (strcmp(m, "tools/list") == 0) {
		gbuf lb;
		gbuf_init(&lb);
		tools_list_json(&lb);
		if (lb.failed || !lb.p) {
			gbuf_free(&lb);
			jsonrpc_error(resp, idraw, idlen, -32603, "internal error");
			jv_free(root);
			return 1;
		}
		jsonrpc_result(resp, idraw, idlen, lb.p);
		gbuf_free(&lb);
	} else if (strcmp(m, "tools/call") == 0) {
		jv *params = jv_get(root, "params");
		jv *args = params ? jv_get(params, "arguments") : NULL;
		jv *name = params ? jv_get(params, "name") : NULL;
		if (!name || name->t != 3 || !name->str || !name->str[0]) {
			jsonrpc_error(resp, idraw, idlen, -32602, "invalid tool call arguments");
			jv_free(root);
			return 1;
		}
		gbuf txt;
		gbuf_init(&txt);
		int is_error = 0;
		if (tools_call(name->str, args, &txt, &is_error) != 0) {
			jsonrpc_error(resp, idraw, idlen, -32602, "unknown tool or invalid arguments");
			gbuf_free(&txt);
			jv_free(root);
			return 1;
		}
		if (txt.failed || !txt.p) {
			jsonrpc_error(resp, idraw, idlen, -32603, "internal error");
			gbuf_free(&txt);
			jv_free(root);
			return 1;
		}
		/* Bound the result: truncate loudly rather than emitting
		 * megabytes. The mark goes inside the text so the envelope
		 * stays exactly the MCP shape. */
		if (txt.len > SBM_MAX_RESULT_BYTES) {
			txt.len = SBM_MAX_RESULT_BYTES;
			txt.p[txt.len] = 0;
			gbuf_adds(&txt, "\n[truncated at 256KiB]");
		}
		gbuf out;
		gbuf_init(&out);
		gbuf_adds(&out, "{\"content\":[{\"type\":\"text\",\"text\":");
		json_escape(&out, txt.p, txt.len);
		gbuf_adds(&out, "}],\"isError\":");
		gbuf_adds(&out, is_error ? "true}" : "false}");
		gbuf_free(&txt);
		if (out.failed) {
			gbuf_free(&out);
			jsonrpc_error(resp, idraw, idlen, -32603, "internal error");
			jv_free(root);
			return 1;
		}
		/* Bypass jsonrpc_result: the envelope is already assembled. */
		gbuf_adds(resp, "{\"jsonrpc\":\"2.0\",\"id\":");
		gbuf_addn(resp, idraw, idlen);
		gbuf_adds(resp, ",\"result\":");
		gbuf_adds(resp, out.p);
		gbuf_adds(resp, "}");
		gbuf_free(&out);
	} else {
		jsonrpc_error(resp, idraw, idlen, -32601, "method not found");
	}
	jv_free(root);
	return 1;
}

/* ------------------------------------------------------------------ */
/* stdio transport: NDJSON primary, Content-Length fallback            */
/* ------------------------------------------------------------------ */
static int find_content_length(const char *h, size_t hl, size_t *outlen) {
	size_t i, p, start, v;
	*outlen = 0;
	for (i = 0; i + 15 < hl; i++) {
		if (ascii_ncasecmp(h + i, "content-length:", 15) != 0)
			continue;
		p = i + 15;
		while (p < hl && (h[p] == ' ' || h[p] == '\t'))
			p++;
		v = 0;
		start = p;
		while (p < hl && isdigit((unsigned char)h[p])) {
			if (v > (SIZE_MAX - (size_t)(h[p] - '0')) / 10)
				return -1;
			v = v * 10 + (size_t)(h[p] - '0');
			p++;
		}
		if (p == start || (p < hl && h[p] != '\r' && h[p] != '\n' &&
		    h[p] != ' ' && h[p] != '\t'))
			return -1;
		*outlen = v;
		return 1;
	}
	return 0;
}

/* Extract one complete JSON-RPC message from the input buffer.
 * NDJSON (one object per line) is primary; LSP-style Content-Length
 * framing is accepted as a fallback. Returns 1 with the message in
 * body, 0 when more input is needed, -1 on oversize (fatal).
 * Never blocks: the caller feeds bytes as they arrive. */
static int read_one_message(gbuf *in, gbuf *body, int *ndjson) {
	size_t i, nl, hs, hl;
	int found;

	if (in->len > SBM_MAX_REQUEST_BYTES + SBM_MAX_HEADER_BYTES)
		return -1;
	i = 0;
	while (i < in->len && (in->p[i] == ' ' || in->p[i] == '\t'))
		i++;
	*ndjson = (i < in->len && in->p[i] == '{');

	if (*ndjson) {
		size_t end;
		nl = i;
		while (nl < in->len && in->p[nl] != '\n')
			nl++;
		if (nl >= in->len)
			return 0; /* partial line: wait for the newline */
		/* Tolerate CRLF senders (Windows consoles/pipes): a
		 * single trailing carriage return is framing, not
		 * content. Our own output never emits it. nl still
		 * marks the newline for the buffer surgery below. */
		end = (nl > i && in->p[nl - 1] == '\r') ? nl - 1 : nl;
		gbuf_addn(body, in->p + i, end - i);
		memmove(in->p, in->p + nl + 1, in->len - (nl + 1));
		in->len -= (nl + 1);
		in->p[in->len] = 0;
		return 1;
	}

	/* Non-'{' input: LSP header only if it actually names
	 * Content-Length; otherwise the line is invalid NDJSON and must
	 * still produce a JSON-RPC error (never silent). Raw length is
	 * preserved so oversize whitespace trips the 64 KiB limit. */
	if (i + 15 <= in->len &&
	    ascii_ncasecmp(in->p + i, "content-length:", 15) == 0) {
		found = 0;
		hl = 0;
		for (hs = 0; hs + 1 < in->len; hs++) {
			if (in->p[hs] == '\r' && in->p[hs + 1] == '\n') {
				if (hs + 3 < in->len && in->p[hs + 2] == '\r' &&
				    in->p[hs + 3] == '\n') {
					hl = hs + 4;
					found = 1;
					break;
				}
			} else if (in->p[hs] == '\n' && in->p[hs + 1] == '\n') {
				hl = hs + 2;
				found = 1;
				break;
			}
		}
		if (!found)
			return 0;
		if (hl > SBM_MAX_HEADER_BYTES)
			return -1;
		{
			size_t clen = 0;
			int cl = find_content_length(in->p, hl, &clen);
			if (cl != 1 || clen == 0 || clen > SBM_MAX_REQUEST_BYTES)
				return -1;
			if (clen > SIZE_MAX - hl || in->len < hl + clen)
				return 0;
			gbuf_addn(body, in->p + hl, clen);
			memmove(in->p, in->p + hl + clen, in->len - (hl + clen));
			in->len -= (hl + clen);
			in->p[in->len] = 0;
			return 1;
		}
	}

	nl = 0;
	while (nl < in->len && in->p[nl] != '\n')
		nl++;
	if (nl >= in->len)
		return 0; /* partial line: wait for the newline */
	{
		size_t bl = nl;
		if (bl > 0 && in->p[bl - 1] == '\r')
			bl--;
		gbuf_addn(body, in->p, bl);
	}
	memmove(in->p, in->p + nl + 1, in->len - (nl + 1));
	in->len -= (nl + 1);
	in->p[in->len] = 0;
	*ndjson = 1;
	return 1;
}

static int write_all(int fd, const char *p, size_t len) {
	size_t off = 0;
	while (off < len) {
		ssize_t n = SBM_WRITE(fd, p + off, len - off);
		if (n > 0)
			off += (size_t)n;
		else if (n < 0 && errno == EINTR)
			continue;
		else
			return 0;
	}
	return 1;
}

int run_stdio(void) {
	gbuf in;
	char t[8192];
#ifdef _WIN32
	/* The MCP protocol is byte-exact NDJSON: without binary mode
	 * the CRT translates \n to \r\n on stdout and back on stdin,
	 * corrupting framing. stderr stays text (diagnostics only). */
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
#endif
	gbuf_init(&in);
	for (;;) {
		ssize_t n = SBM_READ(0, t, sizeof t);
		if (n <= 0)
			break;
		gbuf_addn(&in, t, (size_t)n);
		if (in.failed) {
			gbuf_free(&in);
			return 1;
		}
		for (;;) {
			gbuf body, resp;
			int ndjson = 0, m;
			gbuf_init(&body);
			m = read_one_message(&in, &body, &ndjson);
			if (m < 0) {
				static const char too_big[] =
				    "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32600,\"message\":\"request too large\"}}\n";
				gbuf_free(&body);
				write_all(1, too_big, sizeof too_big - 1);
				gbuf_free(&in);
				return 1;
			}
			if (m <= 0) {
				gbuf_free(&body);
				break;
			}
			gbuf_init(&resp);
			if (handle_mcp(body.p, body.len, &resp)) {
				int ok;
				if (ndjson) {
					ok = write_all(1, resp.p ? resp.p : "", resp.len) &&
					     write_all(1, "\n", 1);
				} else {
					char hl[64];
					int hlw = snprintf(hl, sizeof hl,
					    "Content-Length: %zu\r\n\r\n", resp.len);
					ok = hlw >= 0 &&
					     write_all(1, hl, (size_t)hlw) &&
					     write_all(1, resp.p ? resp.p : "", resp.len);
				}
				if (!ok) {
					gbuf_free(&body);
					gbuf_free(&resp);
					gbuf_free(&in);
					return 1;
				}
			}
			gbuf_free(&body);
			gbuf_free(&resp);
		}
	}
	gbuf_free(&in);
	return 0;
}
