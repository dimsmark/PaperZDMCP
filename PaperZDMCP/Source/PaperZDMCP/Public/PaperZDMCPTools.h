#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"
#include "PaperZDMCPTools.generated.h"

/** PaperZD asset tools. Edits use editor transactions and remain unsaved until SaveAsset is called. */
UCLASS()
class PAPERZDMCP_API UPaperZDMCPTools : public UToolsetDefinition
{
    GENERATED_BODY()
public:
    /** Report plugin versions and supported operations. Returns JSON. */
    UFUNCTION(meta = (AICallable), Category = "PaperZD")
    static FString GetCapabilities();

    /** List PaperZD sources, sequences and animation blueprints under a /Game folder, without loading them.
     * Results are sorted by object path. Offset and Limit paginate; Limit is 1..500. */
    UFUNCTION(meta = (AICallable), Category = "PaperZD")
    static FString ListAssets(const FString& Folder = TEXT("/Game"), int32 Offset = 0, int32 Limit = 100);

    /** Inspect a PaperZD asset: source, flipbooks, duration, directions, notify IDs, or blueprint graph nodes/pins/links.
     * AssetPath may be a package path (/Game/Anim/Idle) or object path (/Game/Anim/Idle.Idle). */
    UFUNCTION(meta = (AICallable), Category = "PaperZD")
    static FString InspectAsset(const FString& AssetPath);

    /** Create a flipbook animation source. AssetPath must be an unused /Game package path (no dot suffix). */
    UFUNCTION(meta = (AICallable), Category = "PaperZD")
    static FString CreateAnimationSource(const FString& AssetPath);

    /** Create a PaperZD flipbook sequence belonging to SourcePath. All flipbooks must exist and have nonzero duration.
     * Supply one flipbook, or 2..16 equally spaced directional flipbooks. AngleOffset is -45..45 degrees.
     * Does not import textures or create Paper2D flipbooks. */
    UFUNCTION(meta = (AICallable), Category = "PaperZD")
    static FString CreateAnimationSequence(const FString& AssetPath, const FString& SourcePath,
        const TArray<FString>& FlipbookPaths, const FString& Category = TEXT("Default"), float AngleOffset = 0.0f);

    /** Replace the flipbooks of an existing sequence; preserves composite layers and mirroring per retained direction.
     * Direction count must remain the same to avoid silently discarding directional data.
     * Rejects a shorter duration if existing notifies would fall outside it. */
    UFUNCTION(meta = (AICallable), Category = "PaperZD")
    static FString SetSequenceFlipbooks(const FString& AssetPath, const TArray<FString>& FlipbookPaths,
        float AngleOffset = 0.0f);

    /** Create a PaperZD animation blueprint with its default graph for SourcePath. Does not author states or transitions. */
    UFUNCTION(meta = (AICallable), Category = "PaperZD")
    static FString CreateAnimationBlueprint(const FString& AssetPath, const FString& SourcePath);

    /** Compile a PaperZD animation blueprint and return compiler errors/warnings. Does not save. */
    UFUNCTION(meta = (AICallable), Category = "PaperZD")
    static FString CompileAnimationBlueprint(const FString& AssetPath);

    /** Add a custom event notify and register its name on the owning source if needed.
     * Time must be within sequence duration. TrackIndex can select an existing track or append the next track.
     * Returns a notify object name suitable for RemoveNotify. Compile consuming blueprints to refresh notify functions. */
    UFUNCTION(meta = (AICallable), Category = "PaperZD")
    static FString AddCustomNotify(const FString& AssetPath, const FString& NotifyName, float Time, int32 TrackIndex = 0);

    /** Remove exactly one notify by the object name returned by InspectAsset/AddCustomNotify (not the display name).
     * Keeps source-level notify registration because other sequences may use it. */
    UFUNCTION(meta = (AICallable), Category = "PaperZD")
    static FString RemoveNotify(const FString& AssetPath, const FString& NotifyId);

    /** Save only this PaperZD asset's package. Does not save its source, flipbooks or unrelated dirty assets.
     * Refuses animation blueprints with unresolved compile status. Saving persists changes beyond Undo. */
    UFUNCTION(meta = (AICallable), Category = "PaperZD")
    static FString SaveAsset(const FString& AssetPath);

    /** Export the graph document schema, condition language, and owner-binding vocabulary. */
    UFUNCTION(meta = (AICallable), Category = "PaperZD|Graph")
    static FString GetAnimationGraphSchema();

    /** Apply the exact document returned by InspectAsset for an animation blueprint.
     * Validates and compiles a transient copy before changing the asset. Replace must be true for an existing graph.
     * One editor Undo step for changes; equivalent documents are no-ops. No automatic saving.
     * Unchanged custom-rule stubs preserve their original graphs; edits to stubs fail.
     * Returns compile diagnostics with node IDs and document paths; GraphJson is the full graph document. */
    UFUNCTION(meta = (AICallable), Category = "PaperZD|Graph")
    static FString ApplyAnimationGraph(const FString& AssetPath, const FString& GraphJson, bool Replace = false);
};
