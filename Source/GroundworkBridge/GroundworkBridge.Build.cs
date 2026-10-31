using UnrealBuildTool;

public class GroundworkBridge : ModuleRules
{
	public GroundworkBridge(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "Landscape" });
		PrivateDependencyModuleNames.AddRange(new[] { "UnrealEd", "HTTPServer", "Json", "Sockets", "Foliage", "Water" });
	}
}
