"""Semantic comparison for paperzd-graph/1 documents (Python standard library).

    python scripts/graph_document.py before.json after.json
    python scripts/graph_document.py before.json --canonical

Ignores editor layout, GUIDs and list order. States are scoped by machine name;
transitions are keyed by source, destination and priority. Disconnected duplicate
animation nodes retain multiplicity. Custom rules compare their opaque hashes.
"""
import argparse
import json
from pathlib import Path


def canonical(document):
    if document.get("schema") != "paperzd-graph/1":
        raise ValueError("Unsupported document schema")
    ids = {}

    def label(node, path):
        key = node["id"]
        if key in ids:
            raise ValueError(f"Duplicate ID: {key}")
        ids[key] = path

    def animation(graph, path):
        if "unsupported" in graph:
            return
        label(graph["root"], path + "/root")
        nodes = graph["nodes"]
        by_id = {n["id"]: n for n in nodes}
        roles = {}

        def connected(key, role, active):
            if not key:
                return
            if key in active:
                raise ValueError("Animation cycle")
            node = by_id[key]
            roles.setdefault(key, role)
            if node["type"] == "override_slot":
                connected(node["input"], role + "/input", active | {key})

        connected(graph["output"], "output", set())
        counts = {}
        for n in sorted(nodes, key=lambda n: json.dumps({k: v for k, v in n.items() if k not in {"id", "x", "y", "input", "machine"}}, sort_keys=True)):
            kind = n["type"]
            if kind == "state_machine":
                role = "machine:" + n["name"]
            elif kind == "override_slot":
                role = "slot:" + n["slot"]
            else:
                role = roles.get(n["id"], "disconnected:" + n.get("sequence", kind))
            count = counts.get(role, 0)
            counts[role] = count + 1
            scoped = f"{path}/{role}#{count}"
            label(n, scoped)
            if kind == "state_machine":
                machine(n["machine"], scoped)

    def machine(graph, path):
        label(graph["entry"], path + "/entry")
        names = {}
        for n in graph["nodes"]:
            role = n["type"] + ":" + n["name"]
            if role in names.values():
                raise ValueError("Duplicate state-machine name")
            names[n["id"]] = role
            label(n, path + "/" + role)
            if n["type"] == "state":
                animation(n["graph"], path + "/" + role + "/animation")
        seen = set()
        for t in graph["transitions"]:
            role = f'{names[t["from"]]}->{names[t["to"]]}@{t["priority"]}'
            if role in seen:
                raise ValueError("Duplicate transition (from, to, priority)")
            seen.add(role)
            label(t, path + "/transition:" + role)
            if t["transitional_animation"] is not None:
                animation(t["transitional_animation"], path + "/transition:" + role + "/animation")

    animation(document["graph"], "graph")

    def normalize(value, field=None):
        if isinstance(value, dict):
            return {k: normalize(v, k) for k, v in sorted(value.items()) if k not in {"id", "x", "y"}}
        if isinstance(value, list):
            return sorted((normalize(v) for v in value), key=lambda v: json.dumps(v, sort_keys=True))
        if isinstance(value, str) and field in {"output", "input", "entry_state", "from", "to", "target", "player", "animation_finished"}:
            return ids[value] if value else ""
        # Playback settings are native floats, regardless of JSON precision.
        if isinstance(value, (float, int)) and field in {"play_rate", "start_position"}:
            return float(format(value, ".7g"))
        return value

    return normalize(document)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", nargs="?", type=Path)
    parser.add_argument("--canonical", action="store_true")
    args = parser.parse_args()
    before = canonical(json.loads(args.before.read_text(encoding="utf-8-sig")))
    if args.canonical:
        print(json.dumps(before, indent=2, sort_keys=True))
        return 0
    if not args.after:
        parser.error("supply after.json or --canonical")
    after = canonical(json.loads(args.after.read_text(encoding="utf-8-sig")))
    equal = before == after
    print(json.dumps({"semantically_equal": equal}))
    return 0 if equal else 1


if __name__ == "__main__":
    raise SystemExit(main())
