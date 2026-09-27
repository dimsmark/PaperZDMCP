# Validation: 0.2.1 beta

Validated on 2026-09-27. Earlier asset-tool evidence is retained in [0.1.0 validation](VALIDATION-0.1.0.md).

| Check | Result |
| --- | --- |
| Unreal Engine | 5.8.2, build 56702186, Windows x64 |
| PaperZD | 2.2.4, public source revision `91439935af58854ea741691e8bfb2535ff9c0a4b` |
| Development Editor C++ build | Passed |
| `PaperZDMCP.RegistrationAndSchema` | Passed |
| `PaperZDMCP.AssetWorkflow` | Passed |
| `PaperZDMCP.GraphRoundTrip` | Passed |
| `PaperZDMCP.OwnerPersistence` | Passed, including PIE/SIE and package reload |
| Cold-start owner persistence | Passed in a second editor process; previously unloaded saved fixture |
| Unreal automation totals | 4 successful tests plus 1 cold-start test, 0 failed; expected invalid-input/headless viewport warnings |
| Python semantic comparison/schema consistency suite | 6 tests passed |
| Real exported graph versus cloned graph | Semantically equal |
| Real exported graph against public JSON Schema | Passed |
| Native MCP schema/apply/save/repeat/error test | Passed using a standard-library HTTP client |

Owner persistence uses a four-property Blueprint actor fixture (float/double and bool properties). Inspection creates no Blueprint objects and leaves class ownership, package dirtiness and Undo unchanged. The expected owner description is plain data, with no temporary Blueprint or graph. Generated functions survive reconstructed cast/call nodes, reordered node/pin arrays, replaced GUIDs, moved nodes, cosmetic comments and empty false literals. Stale 0.2.0 fingerprints are ignored. Changing a getter, setter, cast, called function, literal or execution link is rejected without rewriting the graph.

Defaults are checked for explicit and implicit zero, true, nonzero and negative real values, resetting a nonzero value to implicit zero, subsequent full compiles and exact Undo restoration. The seed editor runs an actual in-process PIE/SIE world, stops it and saves. A second OS process verifies a different PID, asserts the fixture was not already loaded, then loads it and matches the complete saved export exactly. Its first apply is a clean no-op with unchanged Undo; subsequent edits compile, save and reload successfully. This is an editor process restart test, not just package reload.

The graph fixture exercises nested machines with the same `Idle` state and `Reset` jump names; entries, states, a conduit, an override slot and root wiring; transition priorities and transitional animation; bool/number comparisons, AND/OR/NOT, a pure Blueprint function, animation finished and time-remaining ratio bindings. Eight compiled predicate truth-table cases pass. The generated update function also reads a cached character property at runtime.

Safety checks cover clean-package idempotence and unchanged Undo length for original requests and inspected documents; exact JSON restoration after one Undo; state rename with retained transitions; cloning to another compatible AnimBP; two-player and wrong-source getter rejection; missing owner class/property and callback rejection; actual compiler errors with source-node diagnostics; and retained hand-authored lifecycle event chains.

An unsupported custom rule exports an opaque stub. Applying another change preserves its original graph object and serialized node text byte-for-byte. Repeating that document is a no-op. Altering the hash fails without changing the export, dirty flag or Undo history.

Native MCP testing retrieved the exact public schema, created an AnimBP with cached owner updates, applied and compiled it, saved it, then repeated the original request. The second request returned `changed: false` and `dirty: false`. The transport test also changes bool/real defaults, compiles separately and reads the values back from inspection. An invalid owner property returned native `isError: true`, and inspection remained unchanged. This test ran against a disposable editor on port 18080, not a game project.

The native toolset is `PaperZDMCP.PaperZDMCPTools`, with 13 functions. Native MCP advertises lower-camel-case arguments. Text results are wrapped in a `returnValue` field containing the extension's JSON response.

Tests ran with NullRHI in a disposable project. The headless process required `HIGHDPIAWARE` compatibility on a scaled portrait display to avoid Unreal's minimum-resolution startup check. The test script includes this child-process setting and restores the calling environment afterward.

## Remaining release limits

- Source beta, not a precompiled cross-platform release.
- No direct Claude UI session was tested. Standard MCP transport was exercised independently; the extension contains no client-specific API.
- PIE/SIE lifecycle was exercised with NullRHI; no rendered gameplay or character-setup test was performed.
- PaperZD 2.2.4 has no transition blend settings. `blend` must be null.
- Unsupported animation nodes (including SelectBy/caching/layering), externally driven playback pins, and unsupported variable types block graph application explicitly.
- Owner bindings currently read bool/real properties. Arbitrary owner getter chains, cache-class changes and removal of existing variables/hooks are outside this version.
- Custom rule stubs preserve rules on their original asset; copying arbitrary custom Blueprint implementations between assets is unsupported.
- Character/component setup and AnimBP assignment remain outside this toolset.
- Other Unreal/PaperZD versions and macOS/Linux builds are unverified.
- Source-control checkout, asset-import pipelines, and destructive asset deletion are not provided.

Local build logs, the Unreal automation JSON report, and native MCP test evidence are retained under `.build/` in the development workspace. They are excluded from distributable archives.
