#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "GroundworkLandscapeLibrary.generated.h"

class ALandscape;
class UMaterialInterface;

/** Landscape helpers for scripts (Python cannot make a landscape of any size). */
UCLASS()
class UGroundworkLandscapeLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * A new landscape from a raw heightmap (little-endian uint16, row by row, (ComponentsX * QuadsPerComponent + 1) by
	 * (ComponentsY * QuadsPerComponent + 1) values; 32768 = the landscape's own zero), one section per component, its
	 * corner at Location, as the editor's New Landscape button makes one. Null when the file does not fit.
	 */
	UFUNCTION(BlueprintCallable, Category = "Groundwork", meta = (WorldContext = "WorldContext"))
	static ALandscape* CreateLandscapeFromRaw(UObject* WorldContext, const FString& RawFile, int32 ComponentsX, int32 ComponentsY, int32 QuadsPerComponent,
		FVector Location, FVector Scale, UMaterialInterface* Material, const FString& Label);

	/**
	 * Old's replacement at a new size, as the editor's own landscape resize makes one: Old's shared properties (material,
	 * LODs, virtual textures...) and edit layers with their brushes (the Water plugin's brush manager) carried over, the
	 * brushes pointed at the new landscape, then Old (and its streaming proxies) deleted. Heights from RawFile as for
	 * CreateLandscapeFromRaw. Null when the file does not fit (Old is then left alone).
	 */
	UFUNCTION(BlueprintCallable, Category = "Groundwork")
	static ALandscape* ReplaceLandscapeFromRaw(ALandscape* Old, const FString& RawFile, int32 ComponentsX, int32 ComponentsY, int32 QuadsPerComponent, FVector Location);
};
