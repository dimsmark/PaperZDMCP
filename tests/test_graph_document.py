"""Run with python -m unittest discover -s tests -p 'test_*.py'."""
import copy
import importlib.util
import json
from pathlib import Path
import random
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("graph_document", ROOT / "scripts/graph_document.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def identity(**fields):
    return dict(id=str(uuid.uuid4()), x=0, y=0, **fields)


def animation():
    player = identity(type="play_sequence", sequence="/Game/Idle.Idle", loop=False, play_rate=1, start_position=0)
    return dict(root=identity(), output=player["id"], nodes=[player])


def machine(name):
    idle = identity(type="state", name="Idle", graph=animation(), OnStateEnterEventName="None", OnStateExitEventName="None")
    run = identity(type="state", name="Run", graph=animation(), OnStateEnterEventName="None", OnStateExitEventName="None")
    transition = identity(**{"from": idle["id"], "to": run["id"]}, priority=0, blend=None, rule={"variable": "Moving"}, transitional_enabled=False, transitional_animation=None, color="FFFFFFFF")
    jump = identity(type="jump", name="Reset", target=idle["id"])
    return identity(type="state_machine", name=name, machine=dict(entry=identity(), entry_state=idle["id"], nodes=[idle, run, jump], transitions=[transition]))


def fixture():
    first, second = machine("Locomotion"), machine("Secondary")
    nested = identity(type="state", name="Nested", graph=dict(root=identity(), output=second["id"], nodes=[second]), OnStateEnterEventName="None", OnStateExitEventName="None")
    first["machine"]["nodes"].append(nested)
    return dict(schema="paperzd-graph/1", animation_source="/Game/Source.Source", variables=[dict(name="Moving", type="bool", default="false")], owner_update=None,
                graph=dict(root=identity(), output=first["id"], nodes=[first]))


class SemanticTests(unittest.TestCase):
    def test_guid_layout_and_list_order_do_not_matter(self):
        before = fixture()
        ids = {}

        def visit(value):
            if isinstance(value, dict):
                if "id" in value:
                    ids[value["id"]] = str(uuid.uuid4())
                for child in value.values():
                    visit(child)
            elif isinstance(value, list):
                for child in value:
                    visit(child)

        visit(before)

        def rewrite(value):
            if isinstance(value, dict):
                return {k: (123 if k in {"x", "y"} else rewrite(v)) for k, v in reversed(list(value.items()))}
            if isinstance(value, list):
                return [rewrite(v) for v in reversed(value)]
            return ids.get(value, value) if isinstance(value, str) else value

        self.assertEqual(module.canonical(before), module.canonical(rewrite(before)))

    def test_meaningful_changes_are_detected(self):
        before = fixture()
        for field, value in [("priority", 9), ("rule", {"constant": True}), ("transitional_enabled", True)]:
            after = copy.deepcopy(before)
            after["graph"]["nodes"][0]["machine"]["transitions"][0][field] = value
            self.assertNotEqual(module.canonical(before), module.canonical(after))

    def test_same_names_in_distinct_machines_are_distinct(self):
        before = fixture()
        after = copy.deepcopy(before)
        after["graph"]["nodes"][0]["machine"]["nodes"][3]["graph"]["nodes"][0]["machine"]["nodes"][0]["name"] = "Rest"
        self.assertNotEqual(module.canonical(before), module.canonical(after))

    def test_duplicate_transition_tuple_is_rejected(self):
        value = fixture()
        transitions = value["graph"]["nodes"][0]["machine"]["transitions"]
        duplicate = copy.deepcopy(transitions[0]); duplicate["id"] = str(uuid.uuid4()); transitions.append(duplicate)
        with self.assertRaisesRegex(ValueError, "Duplicate transition"):
            module.canonical(value)

    def test_opaque_hash_is_significant(self):
        before = fixture(); before["graph"]["nodes"][0]["machine"]["transitions"][0]["rule"] = dict(kind="custom", hash="original", reason="unsupported")
        after = copy.deepcopy(before); after["graph"]["nodes"][0]["machine"]["transitions"][0]["rule"]["hash"] = "changed"
        self.assertNotEqual(module.canonical(before), module.canonical(after))

    def test_embedded_schema_matches_public_schema(self):
        import re
        source = (ROOT / "PaperZDMCP/Source/PaperZDMCP/Private/PaperZDMCPGraphSchema.inl").read_text()
        embedded = json.loads("".join(re.findall(r'R"JSON\((.*?)\)JSON"', source, flags=re.S)))
        self.assertEqual(embedded, json.loads((ROOT / "docs/graph.schema.json").read_text()))


if __name__ == "__main__":
    unittest.main()
