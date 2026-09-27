# Development and validation

`PaperZDMCP` is a normal Unreal editor module. `UPaperZDMCPTools` derives from `UToolsetDefinition`; the module registers it at `PostEngineInit` and unregisters it at shutdown. Static `AICallable` functions become native MCP tools. No standalone transport or client-specific dependency is used.

Only PaperZD public headers are included. Flipbook assignment uses the reflected animation-data array because PaperZD exposes a const getter. The entry layout is checked before changing assets. This keeps per-direction composite layers and mirror settings intact during replacement. This release targets the `AnimData` struct layout in PaperZD 2.2.4.

## Build and test on Windows

From the repository root, pass your own dependency locations:

```powershell
./scripts/test.ps1 -EngineRoot 'C:/Epic/UE_5.8' -PaperZDPath 'C:/MyProject/Plugins/PaperZD'
```

The script copies source into a fresh `.build/test-<id>` host, compiles the editor, and runs `PaperZDMCP.*` automation tests with NullRHI. It does not install into or edit the supplied project/plugin. Logs, reports and disposable test assets remain inside that host for review.

`PaperZDMCP.OwnerPersistence` creates a Blueprint owner with four properties and tests generated functions, CDO defaults, reconstruction, Undo, manual-edit protection, package reload and an in-process PIE/SIE cycle. It saves a fixture and manifest. After that editor exits, the script launches a new editor process with `-PaperZDMCPColdStart` and runs this test again. The second process asserts a different PID, loads the previously unloaded asset, compares its exact saved export, checks a clean no-op and then applies further edits. Both automation reports are required; process exit codes alone do not establish passing tests.

Tests cover native toolset registration/schema export; source, sequence and blueprint creation; collisions and invalid inputs; directional sequences; notify registration/removal; replacement duration checks; Undo; compilation; pagination; and explicit source-package saving. The asset workflow refuses to run unless Unreal was launched with `-PaperZDMCPScratchProject`, since it writes a uniquely named test source under `/Game/PaperZDMCPTest_<id>`.

These are editor automation tests, not rendered gameplay tests. They do not establish visual correctness of supplied flipbooks or full character animation behavior.

`PaperZDMCP.GraphRoundTrip` additionally exercises nested machines, rule generation and compiled truth tables, cached-owner updates, exact Undo, no-op applies, custom-rule preservation, clone/rename, and validation/compile failures. The tests retain exported fixture documents in the scratch project's `Saved` directory. UE 5.8's test-only exception wrapper needs an explicit keep-alive reference during skeleton compilation; the fixture provides it.

Run the portable semantic comparison tests with `python -m unittest discover -s tests -p 'test_*.py'`. Regenerate the public and embedded JSON schemas together with `python scripts/build_graph_schema.py` after any contract change. Schema tests check that those two copies match.

## Native MCP transport check

Launch the disposable editor with a separate port when another project is open:

```text
UnrealEditor-Cmd.exe <TestHost.uproject> -unattended -nullrhi -nosound -ModelContextProtocolStartServer -ModelContextProtocolPort=18080
```

Then run:

```powershell
python scripts/smoke_mcp.py --url http://127.0.0.1:18080/mcp
```

This performs MCP initialization, tool discovery, schema lookup and a read-only capability call. Requests are sequential, matching native MCP's editor-thread execution model.

For the graph transport test, **only against the disposable TestHost**:

```powershell
python tests/mcp_graph_smoke.py --url http://127.0.0.1:18080/mcp --allow-scratch-writes
```

This creates and saves uniquely named scratch assets, checks graph application/compilation, verifies that repeating the original document leaves the saved package clean, and checks native error propagation without modifying a rejected document's asset.

## Release

1. Build and run automation tests against the intended engine and PaperZD versions.
2. Run the HTTP smoke test. Inspect newly authored assets in the editor before promoting a beta to production.
3. Record evidence and limitations in `docs/VALIDATION.md`.
4. Update the descriptor version and changelog, then run `scripts/package.ps1`.
5. Inspect the archive manifest and verify the `.sha256` file before publishing.

Source distribution is the default. If offering precompiled binaries later, build and label each artifact for the exact Unreal version and platform; do not assume binary compatibility across engine versions. Do not redistribute engine or PaperZD files in this extension's release.
