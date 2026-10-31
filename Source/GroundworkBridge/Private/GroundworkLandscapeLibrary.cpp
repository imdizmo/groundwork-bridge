#include "GroundworkLandscapeLibrary.h"

#include "Engine/World.h"
#include "Landscape.h"
#include "LandscapeBlueprintBrushBase.h"
#include "LandscapeEditLayer.h"
#include "LandscapeStreamingProxy.h"
#include "EngineUtils.h"
#include "LandscapeInfo.h"
#include "Misc/FileHelper.h"
#include "Modules/ModuleManager.h"


ALandscape* UGroundworkLandscapeLibrary::CreateLandscapeFromRaw(UObject* WorldContext, const FString& RawFile, int32 ComponentsX, int32 ComponentsY, int32 QuadsPerComponent,
	FVector Location, FVector Scale, UMaterialInterface* Material, const FString& Label)
{
	UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	const int32 SizeX = ComponentsX * QuadsPerComponent + 1, SizeY = ComponentsY * QuadsPerComponent + 1;
	TArray<uint8> Bytes;
	if (!World || ComponentsX < 1 || ComponentsY < 1 || QuadsPerComponent < 1 || !FFileHelper::LoadFileToArray(Bytes, *RawFile) || Bytes.Num() != SizeX * SizeY * 2)
	{
		UE_LOG(LogTemp, Error, TEXT("Groundwork: %s is not %d x %d uint16 values"), *RawFile, SizeX, SizeY);
		return nullptr;
	}
	TArray<uint16> Heights;
	Heights.SetNumUninitialized(SizeX * SizeY);
	FMemory::Memcpy(Heights.GetData(), Bytes.GetData(), Bytes.Num());

	TMap<FGuid, TArray<uint16>> HeightDataPerLayers;
	HeightDataPerLayers.Add(FGuid(), MoveTemp(Heights));
	TMap<FGuid, TArray<FLandscapeImportLayerInfo>> MaterialLayerDataPerLayers;
	MaterialLayerDataPerLayers.Add(FGuid(), TArray<FLandscapeImportLayerInfo>());

	ALandscape* Landscape = World->SpawnActor<ALandscape>(Location, FRotator::ZeroRotator);
	Landscape->LandscapeMaterial = Material;
	Landscape->SetActorRelativeScale3D(Scale);
	Landscape->StaticLightingLOD = FMath::DivideAndRoundUp(FMath::CeilLogTwo((SizeX * SizeY) / (2048 * 2048) + 1), (uint32)2);
	Landscape->Import(FGuid::NewGuid(), 0, 0, SizeX - 1, SizeY - 1, 1, QuadsPerComponent, HeightDataPerLayers, nullptr, MaterialLayerDataPerLayers,
		ELandscapeImportAlphamapType::Additive, TArrayView<const FLandscapeLayer>());
	if (ULandscapeInfo* Info = Landscape->GetLandscapeInfo())
	{
		Info->UpdateLayerInfoMap(Landscape);
	}
	if (!Label.IsEmpty())
	{
		Landscape->SetActorLabel(Label);
	}
	return Landscape;
}

ALandscape* UGroundworkLandscapeLibrary::ReplaceLandscapeFromRaw(ALandscape* Old, const FString& RawFile, int32 ComponentsX, int32 ComponentsY, int32 QuadsPerComponent, FVector Location)
{
	const int32 SizeX = ComponentsX * QuadsPerComponent + 1, SizeY = ComponentsY * QuadsPerComponent + 1;
	TArray<uint8> Bytes;
	if (!Old || ComponentsX < 1 || ComponentsY < 1 || QuadsPerComponent < 1 || !FFileHelper::LoadFileToArray(Bytes, *RawFile) || Bytes.Num() != SizeX * SizeY * 2)
	{
		UE_LOG(LogTemp, Error, TEXT("Groundwork: %s is not %d x %d uint16 values"), *RawFile, SizeX, SizeY);
		return nullptr;
	}
	TArray<uint16> Heights;
	Heights.SetNumUninitialized(SizeX * SizeY);
	FMemory::Memcpy(Heights.GetData(), Bytes.GetData(), Bytes.Num());
	TMap<FGuid, TArray<uint16>> HeightDataPerLayers;
	TMap<FGuid, TArray<FLandscapeImportLayerInfo>> MaterialLayerDataPerLayers;
	// Every edit layer gets the heights (as the resize does with one source); the brushes then work on top.
	for (const FLandscapeLayer& Layer : Old->GetLayersConst())
	{
		HeightDataPerLayers.Add(Layer.EditLayer ? Layer.EditLayer->GetGuid() : FGuid(), Heights);
		MaterialLayerDataPerLayers.Add(Layer.EditLayer ? Layer.EditLayer->GetGuid() : FGuid(), TArray<FLandscapeImportLayerInfo>());
	}
	// Import also reads the final (combined) layer's heights under the empty key.
	HeightDataPerLayers.Add(FGuid(), Heights);
	MaterialLayerDataPerLayers.Add(FGuid(), TArray<FLandscapeImportLayerInfo>());

	FActorSpawnParameters SpawnParams;
	SpawnParams.OverrideLevel = Old->GetLevel();
	ALandscape* New = Old->GetWorld()->SpawnActor<ALandscape>(Location, Old->GetActorRotation(), SpawnParams);
	New->CopySharedProperties(Old);
	New->SetActorRelativeScale3D(Old->GetActorScale());
	New->SetLandscapeGuid(FGuid(), /* bValidateGuid= */ false);   // CopySharedProperties stomped it
	New->Import(FGuid::NewGuid(), 0, 0, SizeX - 1, SizeY - 1, 1, QuadsPerComponent, HeightDataPerLayers, *Old->ReimportHeightmapFilePath, MaterialLayerDataPerLayers,
		ELandscapeImportAlphamapType::Additive, Old->GetLayersConst());

	// The brushes (water) to the new landscape, then the old one away.
	TArray<ALandscapeBlueprintBrushBase*> Brushes;
	for (TActorIterator<ALandscapeBlueprintBrushBase> It(Old->GetWorld()); It; ++It)
	{
		Brushes.Add(*It);
	}
	TArray<ALandscapeStreamingProxy*> Proxies;
	for (TActorIterator<ALandscapeStreamingProxy> It(Old->GetWorld()); It; ++It)
	{
		if (It->GetLandscapeActor() == Old)
		{
			Proxies.Add(*It);
		}
	}
	const FString Label = Old->GetActorLabel();
	for (ALandscapeStreamingProxy* Proxy : Proxies)
	{
		Proxy->Destroy();
	}
	Old->Destroy();
	for (ALandscapeBlueprintBrushBase* Brush : Brushes)
	{
		Brush->SetOwningLandscape(New);
	}
	if (ULandscapeInfo* Info = New->GetLandscapeInfo())
	{
		Info->UpdateLayerInfoMap(New);
	}
	New->SetActorLabel(Label);
	New->RequestLayersContentUpdateForceAll();
	UE_LOG(LogTemp, Display, TEXT("Groundwork: landscape replaced, %d x %d, %d brushes moved"), SizeX, SizeY, Brushes.Num());
	return New;
}
