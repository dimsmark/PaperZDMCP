# PaperZD MCP

An editor-only toolset for **Unreal Engine 5.8's native Unreal MCP server**. It exposes PaperZD asset operations to Claude Code and other MCP clients through Unreal's existing HTTP endpoint. No client-specific plugin, Python service, account, or API key is required.

This is a community extension, not an official Epic Games or Critical Failure Studio product. Version 0.2.1 is a source-distributed beta. It does not integrate with unrelated third-party projects also named `unreal-mcp`.

Download the source from [GitHub](https://github.com/dimsmark/PaperZDMCP). See the [validation results and release limits](docs/VALIDATION.md).

## Requirements

- Unreal Engine **5.8**, including the `ModelContextProtocol` and `ToolsetRegistry` plugins.
- PaperZD **2.2.4**, installed separately for that engine version.
- A C++ project and the compiler/SDK required by your Unreal installation. For a Blueprint-only project, add an empty C++ class first.
- An MCP client supporting Streamable HTTP. The editor must be running.

The test environment is Windows x64. Other platforms and engine/PaperZD versions are unverified. Unreal's native MCP API is experimental.

## Install

1. Close the target editor.
2. Copy the `PaperZDMCP` directory into `<YourProject>/Plugins/`, next to your PaperZD installation if it is project-local. The descriptor must be at `Plugins/PaperZDMCP/PaperZDMCP.uplugin`.
3. Enable `PaperZDMCP` in the project's `.uproject` plugin list:

   ```json
   { "Name": "PaperZDMCP", "Enabled": true }
   ```

4. Regenerate project files and build your project's **Development Editor** target. Open the project. The extension declares its PaperZD, Paper2D, Toolset Registry and Unreal MCP dependencies automatically.
5. In the Unreal console, run `ModelContextProtocol.StartServer 8000`. Alternatively enable **Auto Start Server** under **Editor Preferences > General > Model Context Protocol**.
6. Connect the MCP client to `http://127.0.0.1:8000/mcp`.

`AllToolsets` is optional: enable it if you also want Epic's other editor tools. PaperZDMCP registers its own toolset independently.

### Claude Code

Run this in the Unreal console:

```text
ModelContextProtocol.GenerateClientConfig ClaudeCode
```

Or merge this entry into `.mcp.json` in your Unreal project root:

```json
{
  "mcpServers": {
    "unreal-mcp": {
      "type": "http",
      "url": "http://127.0.0.1:8000/mcp"
    }
  }
}
```

Start Claude Code in that project directory. Ask it to discover the `PaperZDMCPTools` toolset and call `GetCapabilities`. Other clients connect to the same endpoint using their own configuration format. This package does not depend on Codex or Claude-specific APIs.

## Tools

| Tool | Purpose |
| --- | --- |
| `GetCapabilities` | Versions, editor availability, supported scope |
| `ListAssets` | Paginated PaperZD asset discovery under `/Game` |
| `InspectAsset` | Source/sequence metadata, or the round-trip graph document for an AnimBP |
| `GetAnimationGraphSchema` | JSON Schema for that document, including the condition language |
| `ApplyAnimationGraph` | Preflight/compile and apply an AnimBP graph in one Undo step |
| `CreateAnimationSource` | New flipbook animation source |
| `CreateAnimationSequence` | New sequence referencing existing flipbooks, including directional sequences |
| `SetSequenceFlipbooks` | Replace flipbooks while retaining per-direction composite and mirror data |
| `CreateAnimationBlueprint` | New PaperZD animation blueprint with its default graph |
| `CompileAnimationBlueprint` | Compile and return diagnostic messages |
| `AddCustomNotify` | Register a custom event name and place it on a sequence |
| `RemoveNotify` | Remove a particular notify instance by its object ID |
| `SaveAsset` | Persist one selected PaperZD asset package |

Functions return JSON text. Mutation responses include `ok`; AnimBP inspection returns the graph document itself, and the schema tool returns JSON Schema. Invalid requests also raise native tool errors. Arguments and defaults are exposed through the native tool schema; do not guess hashed MCP tool names.

When native **Tool Search** is enabled, use `list_toolsets`, then `describe_toolset` with `toolset_name: "PaperZDMCP.PaperZDMCPTools"`. Dispatch through `call_tool`, for example:

```json
{
  "toolset_name": "PaperZDMCP.PaperZDMCPTools",
  "tool_name": "CreateAnimationSource",
  "arguments": { "assetPath": "/Game/Hero/AS_Hero" }
}
```

Use `describe_toolset` to confirm exact field spelling in your installed engine. With Tool Search disabled, tools appear directly in `tools/list`.

### Example workflow

Ask your MCP client:

> Using PaperZDMCPTools, create `/Game/Hero/AS_Hero`, then create `/Game/Hero/AS_Idle` from the existing `/Game/Hero/FB_Idle` flipbook. Create `/Game/Hero/ABP_Hero` using that animation source. Inspect the results and compile the blueprint. Leave everything unsaved for review.

Creation gives the blueprint PaperZD's default graph. Use `GetAnimationGraphSchema`, `InspectAsset`, and `ApplyAnimationGraph` to author playback, nested state machines, transitions, conduits, jump nodes, override slots, and cached owner-variable updates. See [the graph workflow and supported language](docs/GRAPH_DOCUMENT.md). Existing hand-authored custom rule graphs are preserved as opaque stubs. PaperZD 2.2.4 has no transition blend settings; unsupported settings fail explicitly.

Character/component setup and assigning the AnimBP still happen in Unreal or through other editor tools. The extension does not create/import sprites, textures, or Paper2D flipbooks, or author sound/VFX notify classes.

## Editing behavior

- Asset paths are restricted to `/Game`. New assets require a package path such as `/Game/Hero/AS_Idle`; existing assets accept that or the full object path.
- Existing packages are never overwritten by creation tools.
- Graph application requires `replace: true` for existing content, compiles a transient copy before changing the destination, and compiles the destination before reporting success. Repeating an equivalent document adds no Undo entry and does not dirty or rebuild the asset.
- Mutations require Play In Editor to be stopped. Existing-asset edits use Unreal Undo transactions and remain dirty until explicitly saved; compilation follows Unreal's normal compiler behavior. Asset creation is not promised to be reversible through Undo.
- Directional flipbooks must have matching duration and frame rate. Replacement keeps the existing direction count and preserves each retained entry's other fields.
- Replacing flipbooks rejects durations that would cut off existing notifies. Frame-specific mirroring data is retained; inspect its suitability when changing frame layouts.
- Adding a custom notify can also update notify functions in animation blueprints using the source, as part of PaperZD's own editor behavior. Compile those blueprints and save each affected asset explicitly.
- Notify removal uses the instance `id` from inspection, not its display name. Source-level event registration remains because other sequences may use it.
- `SaveAsset` saves only the selected asset's package. Save sources and other referenced assets as needed. It does not check out source-controlled files automatically.
- Asset inspection may load packages. Normal Unreal/PaperZD load-time migration can occur.

## Verify an installation

With the editor and MCP server running:

```powershell
python scripts/smoke_mcp.py --url http://127.0.0.1:8000/mcp
```

This read-only check uses the standard MCP protocol without any AI client. It verifies tool discovery and capability calls. See [development and validation](docs/DEVELOPMENT.md) for the isolated Unreal automation workflow.

## Distribution

```powershell
./scripts/package.ps1
```

The script creates a versioned source ZIP and SHA-256 checksum in `dist`. Its allowlist includes this extension's source, documentation, license, and support scripts. It excludes Unreal Engine, PaperZD, project content, local paths/configuration, downloaded references, and build intermediates. Recipients install their own dependencies and compile locally.

MIT license applies to this extension's original code. Unreal Engine and PaperZD retain their respective licenses and are not bundled. See [LICENSE](LICENSE).

References: [Epic's native MCP documentation](https://dev.epicgames.com/documentation/unreal-engine/unreal-mcp-in-unreal-editor), [PaperZD source](https://github.com/heavybullets/PaperZD).
