"""Generate the public JSON Schema and the identical embedded MCP schema."""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def obj(properties, required=None, **kwargs):
    return dict(type="object", properties=properties, required=list(properties) if required is None else required, additionalProperties=False, **kwargs)


def ref(name):
    return {"$ref": "#/$defs/" + name}


def array(items, **kwargs):
    return dict(type="array", items=items, **kwargs)


def build():
    string = {"type": "string"}
    boolean = {"type": "boolean"}
    number = {"type": "number"}
    nullable = {"type": "null"}
    guid = {"type": "string", "pattern": "^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"}
    link = {"oneOf": [guid, {"const": ""}]}
    coordinate = {"type": "integer", "minimum": -1000000, "maximum": 1000000}
    identity = dict(id=guid, x=coordinate, y=coordinate)
    comparison = dict(op={"enum": [">", ">=", "<", "<=", "==", "!="]}, value=number)
    function = dict(function=string, arguments={"type": "object", "additionalProperties": string})
    defs = {}
    defs["identity"] = obj(identity)
    defs["rule"] = {"oneOf": [
        obj(dict(constant=boolean)), obj(dict(variable=string)), obj(dict(variable=string, **comparison)),
        obj(dict(variable=string, op={"enum": ["==", "!="]}, value=boolean)),
        obj({"not": ref("rule")}),
        obj(dict(all=array(ref("rule"), minItems=2, maxItems=8))),
        obj(dict(any=array(ref("rule"), minItems=2, maxItems=8))),
        obj(dict(time_remaining={"const": True}, player=guid, ratio=boolean, **comparison), required=["time_remaining", "player", "op", "value"]),
        obj(dict(animation_finished=guid)), obj(function), obj(dict(**function, **comparison)),
        obj(dict(kind={"const": "custom"}, hash=string, reason=string)),
    ], "description": "At most 64 expressions, depth 8. Getters require the unique direct Play Sequence in the source state. Custom stubs must remain unchanged on their original node and asset."}
    defs["play_sequence"] = obj(dict(**identity, type={"const": "play_sequence"}, sequence=string, loop=boolean,
                                           play_rate={"type": "number", "minimum": -10000, "maximum": 10000}, start_position={"type": "number", "minimum": 0}))
    defs["state_machine"] = obj(dict(**identity, type={"const": "state_machine"}, name=string, machine=ref("machine")))
    defs["override_slot"] = obj(dict(**identity, type={"const": "override_slot"}, slot=string, group=string, input=guid))
    defs["animation"] = obj(dict(root=ref("identity"), output=link, nodes=array({"oneOf": [ref("play_sequence"), ref("state_machine"), ref("override_slot")]}, maxItems=512)))
    defs["state"] = obj(dict(**identity, type={"const": "state"}, name=string, graph=ref("animation"), OnStateEnterEventName=string, OnStateExitEventName=string))
    defs["conduit"] = obj(dict(**identity, type={"const": "conduit"}, name=string, rule=ref("rule")))
    defs["jump"] = obj(dict(**identity, type={"const": "jump"}, name=string, target=guid))
    defs["transition"] = obj(dict(**identity, **{"from": guid, "to": guid}, priority={"type": "integer", "minimum": -2147483647, "maximum": 2147483647},
                                  blend=dict(nullable, description="PaperZD 2.2.4 does not expose transition blend settings."), rule=ref("rule"), transitional_enabled=boolean,
                                  transitional_animation={"oneOf": [nullable, ref("animation")]}, color={"type": "string", "pattern": "^[0-9A-Fa-f]{8}$"}))
    defs["machine"] = obj(dict(entry=ref("identity"), entry_state=link, nodes=array({"oneOf": [ref("state"), ref("conduit"), ref("jump")]}), transitions=array(ref("transition"))))
    defs["variable"] = obj(dict(name=string, type={"enum": ["bool", "float"]}, default=string))
    defs["owner_update"] = obj({"class": string, "bindings": array(obj(dict(variable=string, property=string)), maxItems=128)}, description="Cast once in OnInit and cache owner; OnTick calls a separate guarded function. Property must exist and be a Blueprint-visible bool/real matching its target variable.")
    return dict(obj(dict(schema={"const": "paperzd-graph/1"}, animation_source=string,
                         variables=array(ref("variable"), maxItems=128), owner_update={"oneOf": [nullable, ref("owner_update")]}, graph=ref("animation"))),
                **{"$schema": "https://json-schema.org/draft/2020-12/schema", "$defs": defs},
                title="PaperZD MCP graph document", description="InspectAsset's AnimBP response is this document. Apply it as GraphJson with Replace=true for existing content. All IDs globally unique; names scoped to graphs; at most 512 nodes, depth 16. Compiler diagnostics accompany apply. Unsupported graphs are exported explicitly and cannot be applied.")


if __name__ == "__main__":
    schema = json.dumps(build(), indent=2) + "\n"
    (ROOT / "docs/graph.schema.json").write_text(schema, encoding="utf-8")
    compact = json.dumps(build(), separators=(",", ":"))
    chunks = [compact[i:i + 2000] for i in range(0, len(compact), 2000)]
    (ROOT / "PaperZDMCP/Source/PaperZDMCP/Private/PaperZDMCPGraphSchema.inl").write_text(
        '// Generated by scripts/build_graph_schema.py.\nFString Schema;\n' +
        ''.join('Schema += TEXT(R"JSON(' + chunk + ')JSON");\n' for chunk in chunks) + 'return Schema;\n', encoding="utf-8")
