"""Read-only MCP installation check using only Python's standard library."""
import argparse
import json
import urllib.request


class Client:
    def __init__(self, url):
        self.url = url
        self.session = None
        self.version = "2025-03-26"
        self.next_id = 0

    def request(self, method, params=None, notification=False):
        body = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            body["params"] = params
        if not notification:
            self.next_id += 1
            body["id"] = self.next_id
        headers = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream"}
        if self.session:
            headers["Mcp-Session-Id"] = self.session
        if method != "initialize":
            headers["MCP-Protocol-Version"] = self.version
        request = urllib.request.Request(self.url, json.dumps(body).encode(), headers)
        with urllib.request.urlopen(request, timeout=30) as response:
            self.session = response.headers.get("Mcp-Session-Id", self.session)
            if notification:
                return None
            if "text/event-stream" in response.headers.get("Content-Type", ""):
                # Stop after our JSON-RPC response; SSE connections may stay open.
                chunks = []
                for line in response:
                    text = line.decode().rstrip("\r\n")
                    if text.startswith("data:"):
                        chunks.append(text[5:].lstrip())
                    elif not text and chunks:
                        message = json.loads("\n".join(chunks))
                        chunks.clear()
                        if message.get("id") == self.next_id:
                            break
                else:
                    raise RuntimeError("SSE closed without the requested response")
            else:
                message = json.load(response)
        if "error" in message:
            raise RuntimeError(message["error"])
        return message["result"]

    def tool(self, name, arguments):
        result = self.request("tools/call", {"name": name, "arguments": arguments})
        if result.get("isError"):
            raise RuntimeError(result)
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="http://127.0.0.1:8000/mcp")
    args = parser.parse_args()
    client = Client(args.url)
    init = client.request("initialize", {
        "protocolVersion": client.version, "capabilities": {},
        "clientInfo": {"name": "paperzd-mcp-smoke", "version": "0.2.0"},
    })
    client.version = init["protocolVersion"]
    client.request("notifications/initialized", notification=True)
    tools = []
    cursor = None
    while True:
        page = client.request("tools/list", {"cursor": cursor} if cursor else {})
        tools.extend(page["tools"])
        cursor = page.get("nextCursor")
        if not cursor:
            break
    names = {tool["name"] for tool in tools}
    if "describe_toolset" in names and "call_tool" in names:
        schema = client.tool("describe_toolset", {"toolset_name": "PaperZDMCP.PaperZDMCPTools"})
        if "CreateAnimationSequence" not in json.dumps(schema) or "ApplyAnimationGraph" not in json.dumps(schema):
            raise RuntimeError("PaperZDMCPTools is missing expected tools")
        capabilities = client.tool("call_tool", {
            "toolset_name": "PaperZDMCP.PaperZDMCPTools", "tool_name": "GetCapabilities", "arguments": {},
        })
    else:
        matches = [tool for tool in tools if "PaperZDMCP" in json.dumps(tool) and "GetCapabilities" in json.dumps(tool)]
        if len(matches) != 1:
            raise RuntimeError("Cannot identify the PaperZDMCP GetCapabilities tool; enable native Tool Search")
        capabilities = client.tool(matches[0]["name"], {})
    if "extension_version" not in json.dumps(capabilities):
        raise RuntimeError(f"Unexpected capabilities response: {capabilities}")
    print(json.dumps({"ok": True, "server": init["serverInfo"], "capabilities": capabilities}, indent=2))


if __name__ == "__main__":
    main()
