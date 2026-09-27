using UnrealBuildTool;

public class PaperZDMCP : ModuleRules
{
    public PaperZDMCP(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "ToolsetRegistry" });
        PrivateDependencyModuleNames.AddRange(new[] {
            "Engine", "UnrealEd", "AssetRegistry", "Json", "Paper2D", "PaperZD",
            "PaperZDEditor", "Kismet", "KismetCompiler", "BlueprintGraph", "Projects"
        });
    }
}
