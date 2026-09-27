# Changelog

## 0.2.1

- Recognize generated owner functions with a data-only expected recipe and normalized operations, connections and literals compared against the declared bindings. Saved fingerprints are no longer written or trusted; unchanged 0.2.0 functions need no migration.
- Read variable defaults from the compiled class default object using property export text. Apply writes defaults back to both the variable description and CDO after compilation; empty inputs reset to explicit type zero.
- Regression coverage for reconstructed nodes, stale legacy fingerprints, actual manual edits, default edits/reset/Undo, clean no-op applies, package reload, PIE start/stop, and saved assets loaded in a separate editor process.
- The test runner now requires the cold-start regression to pass before reporting success.

## 0.2.0

- Versioned AnimBP inspect/edit/apply documents and an embedded JSON Schema.
- Nested state machines, states, conduits, entries, jumps, override slots, playback settings, transition priorities and transitional animations.
- Explicit condition language, including source-bound time/ratio getters and pure AnimInstance functions.
- Hashed opaque custom rules retained without rewriting; altered or missing stubs fail.
- Bool/real variables and owner-property updates, with one owner cast in OnInit and a separate cached-owner update function called from OnTick.
- Transient-copy preflight compilation, one-step Undo, node-linked diagnostics, and idempotent applies.
- Semantic JSON comparison utility and Unreal graph round-trip/runtime/safety acceptance tests.
- PaperZD 2.2.4 has no blend settings; requests for them are explicitly rejected.

## 0.1.0

- Initial native UE 5.8 MCP toolset with 11 PaperZD tools.
- Source/sequence/animation blueprint creation, inspection and compilation.
- Directional flipbook assignment and custom event notify editing.
- Undo transactions, explicit per-asset saves, validation before edits.
- Isolated Unreal automation tests and client-independent HTTP smoke test.
- Source ZIP packaging without engine or PaperZD redistribution.
