# Animation graph round-trip

`InspectAsset` on a PaperZD AnimBP returns the **document itself**, matching [graph.schema.json](graph.schema.json). Obtain the same schema over MCP with `GetAnimationGraphSchema`. Other asset types retain their metadata response. Unreal's MCP transport wraps returned JSON text in `returnValue`.

1. Inspect the AnimBP and keep an original copy of the document.
2. Edit that document. Preserve node IDs when renaming or changing nodes; generate unique lowercase UUIDs for new nodes.
3. Call `ApplyAnimationGraph` with `assetPath`, `graphJson` (the serialized document), and `replace: true` for existing content.
4. Check `ok`, `changed`, and `diagnostics`. Inspect the returned `document` for the canonical result.
5. Review the Blueprint in Unreal. Use `SaveAsset` explicitly when ready.

The tool validates and compiles a transient copy first. A successful change is one editor Undo step. A repeated equivalent document returns `changed: false` without modifying the destination, dirtying its package, or adding an Undo entry. A clean asset remains clean; a previously dirty asset remains dirty. Changed documents require the explicit replace flag. There is no implicit merge.

## Graph structure

The document has `schema`, `animation_source`, `variables`, `owner_update`, and `graph`. Animation graphs contain `root: {id,x,y}`, `output` (the node ID feeding that root), and `nodes`. Nodes may be:

- `play_sequence`: `sequence`, `loop`, `play_rate`, `start_position` in seconds.
- `state_machine`: `name` and a `machine` containing its entry node, `entry_state`, states/conduits/jumps, and transitions. States contain animation graphs, allowing nested machines.
- `override_slot`: `slot`, `group`, and `input` node ID.

States have `name`, `graph`, `OnStateEnterEventName`, and `OnStateExitEventName`. Use `None` for absent callbacks. Named callbacks must already exist on the AnimInstance and take no parameters. Conduits have `name` and `rule`; jumps have `name` and `target` state ID.

Transitions have `from`, `to`, `priority`, `rule`, `blend`, `transitional_enabled`, `transitional_animation`, and RGBA hexadecimal `color`. Higher priorities execute first. Transitions must be unique by `(from, to, priority)`. A transitional animation is a separate animation graph; retain it even while disabled. PaperZD 2.2.4 exposes **no transition blend settings**, so `blend` must be `null`. Non-null blend settings fail explicitly.

All node types also have `id`, `x`, and `y`. IDs are globally unique within the document. Links use IDs, so renaming a state retains its transitions. State and jump names are scoped to each machine; `Idle` and `Reset` may appear in multiple machines. Documents support up to 512 nodes, nesting depth 16, and 1 MB of JSON.

## Conditions

Supported examples:

```json
{"variable":"Moving"}
{"variable":"Speed","op":">","value":10}
{"all":[{"variable":"Moving"},{"not":{"variable":"Crouched"}}]}
{"any":[{"variable":"Falling"},{"constant":false}]}
{"time_remaining":true,"player":"SOURCE-PLAYER-UUID","ratio":true,"op":"<=","value":0.1}
{"animation_finished":"NONLOOPING-SOURCE-PLAYER-UUID"}
{"function":"CanMove","arguments":{}}
{"function":"GetSpeed","arguments":{},"op":">","value":10}
```

Comparisons support `>`, `>=`, `<`, `<=`, `==`, `!=`. Numeric variables are scalar floats/doubles. Bool comparisons also accept `==`/`!=` with a JSON boolean. Function calls must be Blueprint-callable pure AnimInstance methods returning a bool or real; argument values are scalar Blueprint literal strings. Functions must exist on the target AnimBP or its parent. The tool does not copy function implementations.

Time getters require the **sole direct Play Sequence node in the transition's source state**, with its output connected to the state's root. Multiple players, SelectBy routing, nested-machine sources, conduit sources, and players in another state fail instead of guessing. `ratio` defaults to false (seconds). `animation_finished` requires a nonlooping sequence and checks the appropriate end for forward or reverse playback. Rules are limited to 64 expressions, depth 8; AND/OR groups contain 2–8 expressions.

Rules outside this language export as an opaque stub:

```json
{"kind":"custom","hash":"...","reason":"..."}
```

Keep the whole stub unchanged. Applying an unrelated edit retains that original rule graph and its nodes. Edited, omitted, newly created or cross-asset custom stubs fail. Custom rules must be edited or copied through Unreal; their contents are never invented by the MCP tool.

Unsupported animation node types, externally driven playback pins, and unsupported variable types are marked explicitly and block application. This release does not implement SelectBy, animation caching, layered blends, or arbitrary Blueprint graph synthesis.

## Owner variables

```json
{
  "variables":[{"name":"Crouched","type":"bool","default":"false"}],
  "owner_update":{
    "class":"/Script/Engine.Character",
    "bindings":[{"variable":"Crouched","property":"bIsCrouched"}]
  }
}
```

This is a fragment to insert into a full document. `variables` supports bool and real (`float`, created as double for new variables), with Blueprint defaults represented as strings. Existing float precision is retained. Existing variables cannot be omitted or have their type changed because hand-authored graphs may use them.

Defaults are read from the compiled class default object using Unreal's property export-text API. Booleans export as `true` or `false`; a zero real exports as `0.0`. An empty default on input means the type's zero value, including when resetting a previously nonzero value. Apply sets both the variable description and the compiled default object so later compilations preserve the value.

Owner function protection compares normalized node operations, execution/data connections and relevant literals against a data-only recipe built from the stored owner declaration. It ignores editor identities/layout and engine-generated pin metadata. Inspection creates no temporary Blueprint or graph. There is no persisted owner fingerprint; old fingerprints are ignored. An unchanged generated function remains editable after compile, PIE, save/reload and editor restart. A semantic change to the managed functions or lifecycle hooks blocks application and leaves the graphs untouched.

The owner class must exist and derive from Actor; every binding must name an existing Blueprint-visible bool/real property of matching type. For a Blueprint character, use its generated class path, e.g. `/Game/Hero/BP_Hero.BP_Hero_C`.

`OnInit` calls `PaperZDMCP_OwnerInit`, which gets and casts the owner **once**, stores `PaperZDMCP_Owner`, and prints one clear log message if that cast fails. `OnTick` calls `PaperZDMCP_OwnerUpdate`, which checks the cached owner and reads the bound properties. Existing event execution chains continue after these calls. Neither function recasts the owner each frame. The editor-generated `PrintString` failure message is a development diagnostic and is omitted by Unreal in shipping builds.

Manually changed generated functions/hooks are detected and never overwritten silently. Retain owner configuration when applying later documents; removing it or changing its cached class is not supported in this release. No character component setup or AnimBP assignment is performed automatically.

## Compare and clone

Apply an exported document to a fresh compatible AnimBP to clone the graph. Its animation source/sequences and pure functions must be available on the destination. Custom rule stubs cannot be cloned.

For semantic equality, use:

```text
python scripts/graph_document.py before.json after.json
python scripts/graph_document.py before.json --canonical
```

The comparison scopes states by machine, keys transitions by `(from, to, priority)`, resolves ID references to semantic paths, sorts lists, and ignores IDs and editor coordinates. Exact Undo restoration is tested separately. Do not use semantic equality to discard intentional layout edits; apply preserves layout and treats such edits as changes.

Compile errors make the tool call fail. Diagnostics contain severity/message and, when Unreal provides a source node, `node_id` plus `document_path`. Errors in existing EventGraphs or functions may refer to nodes outside the graph document; their node IDs still identify them in Unreal.
