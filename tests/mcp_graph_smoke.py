"""Native MCP graph integration test. Creates and saves scratch assets.

Use only with the disposable TestHost editor, never a production project.
"""
import argparse
import copy
import json
from pathlib import Path
import sys
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from smoke_mcp import Client


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="http://127.0.0.1:18080/mcp")
    parser.add_argument("--allow-scratch-writes", action="store_true", required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    client = Client(args.url)
    init = client.request("initialize", {"protocolVersion": client.version, "capabilities": {}, "clientInfo": {"name": "paperzd-graph-test", "version": "0.2.1"}})
    client.version = init["protocolVersion"]
    client.request("notifications/initialized", notification=True)

    def call(name, arguments, fail=False):
        response = client.request("tools/call", {"name": "call_tool", "arguments": {"toolset_name": "PaperZDMCP.PaperZDMCPTools", "tool_name": name, "arguments": arguments}})
        if fail:
            assert response.get("isError"), response
            return response
        assert not response.get("isError"), response
        text = next(item["text"] for item in response["content"] if item["type"] == "text")
        return json.loads(json.loads(text)["returnValue"])

    schema = call("GetAnimationGraphSchema", {})
    expected = json.loads((Path(__file__).resolve().parents[1] / "docs/graph.schema.json").read_text())
    assert schema == expected
    assert call("GetCapabilities", {})["extension_version"] == "0.2.1"
    root = "/Game/PaperZDMCPSmoke_" + uuid.uuid4().hex
    source, bp = root + "/Source", root + "/ABP_Test"
    assert call("CreateAnimationSource", {"assetPath": source})["ok"]
    assert call("CreateAnimationBlueprint", {"assetPath": bp, "sourcePath": source})["ok"]
    document = call("InspectAsset", {"assetPath": bp})
    document["variables"] = [{"name": "Crouched", "type": "bool", "default": "false"}]
    document["owner_update"] = {"class": "/Script/Engine.Character", "bindings": [{"variable": "Crouched", "property": "bIsCrouched"}]}
    applied = call("ApplyAnimationGraph", {"assetPath": bp, "graphJson": json.dumps(document)})
    assert applied["ok"] and applied["changed"], applied
    assert call("SaveAsset", {"assetPath": source})["ok"]
    assert call("SaveAsset", {"assetPath": bp})["ok"]
    repeated = call("ApplyAnimationGraph", {"assetPath": bp, "graphJson": json.dumps(document), "replace": True})
    assert repeated["ok"] and not repeated["changed"] and not repeated["dirty"], repeated
    inspection = call("InspectAsset", {"assetPath": bp})
    assert inspection["variables"][0]["default"] == "false", inspection
    changed_defaults = copy.deepcopy(inspection)
    changed_defaults["variables"][0]["default"] = "true"
    changed_defaults["variables"].append({"name": "Speed", "type": "float", "default": "37.25"})
    assert call("ApplyAnimationGraph", {"assetPath": bp, "graphJson": json.dumps(changed_defaults), "replace": True})["ok"]
    assert call("CompileAnimationBlueprint", {"assetPath": bp})["ok"]
    inspection = call("InspectAsset", {"assetPath": bp})
    assert {v["name"]: v["default"] for v in inspection["variables"]} == {"Crouched": "true", "Speed": "37.25"}, inspection
    assert call("SaveAsset", {"assetPath": bp})["ok"]
    assert not call("ApplyAnimationGraph", {"assetPath": bp, "graphJson": json.dumps(inspection), "replace": True})["changed"]
    invalid = copy.deepcopy(inspection)
    invalid["owner_update"]["bindings"][0]["property"] = "MissingOwnerProperty"
    failure = call("ApplyAnimationGraph", {"assetPath": bp, "graphJson": json.dumps(invalid), "replace": True}, fail=True)
    assert "property" in json.dumps(failure).lower(), failure
    assert call("InspectAsset", {"assetPath": bp}) == inspection
    result = dict(ok=True, server=init["serverInfo"], asset=bp, schema_matches=True, apply_compiled=True, cdo_defaults_after_compile=True, repeat_changed=repeated["changed"], repeat_dirty=repeated["dirty"], invalid_request_is_error=True, invalid_request_unchanged=True)
    if args.output:
        args.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
