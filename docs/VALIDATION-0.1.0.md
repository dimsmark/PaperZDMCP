# Validation: 0.1.0 beta

Validated on 2026-09-27.

| Check | Result |
| --- | --- |
| Unreal Engine | 5.8.2, build 56702186, Windows x64 |
| PaperZD | 2.2.4, public source revision `91439935af58854ea741691e8bfb2535ff9c0a4b` |
| Development Editor C++ build | Passed |
| `PaperZDMCP.RegistrationAndSchema` | Passed |
| `PaperZDMCP.AssetWorkflow` | Passed; expected warnings from invalid-input rejection cases |
| Automation totals | 2 successful tests, 0 failed |
| Native MCP HTTP discovery/schema/capability call | Passed with a Python standard-library client |
| Create, save and inspect via native MCP | Passed in disposable project |
| Creation collision via native MCP | Returned `isError: true`; existing asset retained |

The asset workflow exercised real Unreal/PaperZD objects, default animation graphs and compilation, source association, single/directional flipbooks, custom notify registration and removal, Undo for flipbook replacement and notify removal, pagination, invalid inputs, and explicit source saving.

The native toolset name observed over MCP is `PaperZDMCP.PaperZDMCPTools`. All 11 functions were exported. Native MCP advertises lower-camel-case argument names such as `assetPath` and `flipbookPaths`; use the advertised schema. Text results are wrapped by Unreal in a `returnValue` field containing the extension's JSON response.

Tests ran with NullRHI in a disposable project. The headless process required `HIGHDPIAWARE` compatibility on a scaled portrait display to avoid Unreal's minimum-resolution startup check. The test script includes this child-process setting and restores the calling environment afterward.

## Remaining release limits

- Source beta, not a precompiled cross-platform release.
- No direct Claude UI session was tested. Standard MCP transport was exercised independently; the extension contains no client-specific API.
- No rendered gameplay or character-setup test was performed.
- State-machine/transition authoring and playback-node wiring are outside this version's scope.
- Other Unreal/PaperZD versions and macOS/Linux builds are unverified.
- Source-control checkout, asset-import pipelines, and destructive asset deletion are not provided.

Local build logs, the Unreal automation JSON report, and native MCP test evidence are retained under `.build/` in the development workspace. They are excluded from distributable archives.
