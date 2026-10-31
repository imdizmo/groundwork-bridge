// Groundwork's live link: a small HTTP endpoint on this computer only (127.0.0.1, a fresh token in
// Saved/GroundworkBridge.json each start). Each request applies one change to the open level at once:
//   GET  /groundwork/ping     {"bridge": "groundwork", "version": 1}
//   POST /groundwork/heights  {"x0","y0" (the map's pixel (0,0) in Unreal metres), "col","row","w","h", "heights": [metres]}
//   POST /groundwork/layers   {"col","row","w","h", "texture" (optional: a texture the landscape material reads, by
//                             name prefix), "layers": [{"name", "channel" (r/g/b/a, optional), "weights": base64 bytes}]}
//                             A layer with a channel goes into that channel of the texture; any other into the
//                             landscape's paint layer of that name.
//   POST /groundwork/objects  {"remove": [{"mesh","x","y","z"} cm], "add": [{"mesh","x","y","z","qx","qy","qz","qw","sx","sy","sz"}]}
//                             (a "marker" id with its "class" and "tag" instead of a mesh: a marker actor)
//   POST /groundwork/water    {"prefix", "rivers": [{"name","top","end","points": [[x,y,width,depth,current]]}],
//                             "lakes": [{"name","level","depth","outline": [[x,y]]}]} (metres): water bodies labelled
//                             prefix + name, rewritten in place, new ones made like the others of their kind, gone ones
//                             deleted
// And the other way, for the editor to follow edits made here by hand:
//   GET  /groundwork/version      {"heights","objects","water"}: counters that go up when someone (not the editor) changes them
//   GET  /groundwork/heights_all  {"x","y","sx","sy","sz" (the landscape's corner and scale, cm), "w","h","data": base64 uint16}
//   GET  /groundwork/objects_all  {"objects": [[mesh, x,y,z, qx,qy,qz,qw, sx,sy,sz]], "markers": [[id, x,y,z, yaw]]}
//   GET  /groundwork/water_all    {"rivers": [{"name","points": [[x,y,z, width,depth,current]]}], "lakes": [{"name","depth","points": [[x,y,z]]}]} (cm)
// Nothing is saved: the level is marked changed, as a brush stroke by hand would.

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "HttpServerModule.h"
#include "IHttpRouter.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "IPAddress.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Algo/AllOf.h"
#include "Misc/Base64.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "Landscape.h"
#include "LandscapeInfo.h"
#include "LandscapeEdit.h"
#include "LandscapeEditLayer.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapeProxy.h"
#include "GameFramework/PlayerStart.h"
#include "WaterBodyActor.h"
#include "WaterBodyComponent.h"
#include "WaterBodyLakeActor.h"
#include "WaterBodyRiverActor.h"
#include "WaterSplineComponent.h"
#include "WaterSplineMetadata.h"

namespace GroundworkBridge
{
	// ponytail: a fixed port; a setting when two projects must be open at once
	static constexpr uint32 Port = 5772;
	static FString Token;
	static TArray<FHttpRouteHandle> Routes;
	static TSharedPtr<IHttpRouter> Router;
	/** Changes made in Unreal (not by the editor's requests): the editor follows them. */
	static int64 HeightsVersion = 0, ObjectsVersion = 0, WaterVersion = 0;
	/** A request from the editor is being applied: its changes are not counted. */
	static bool bApplying = false;
	/** The landscape updates its layers a moment after the editor's change: those are not counted either until then. */
	static double QuietUntil = 0;
	static FDelegateHandle Modified, Added, Deleted, Moved, Undone;

	static UWorld* World() { return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr; }

	static FString TokenFile() { return FPaths::ProjectSavedDir() / TEXT("GroundworkBridge.json"); }

	static TUniquePtr<FHttpServerResponse> Reply(const FString& Text, EHttpServerResponseCodes Code = EHttpServerResponseCodes::Ok)
	{
		TUniquePtr<FHttpServerResponse> R = FHttpServerResponse::Create(Text, TEXT("text/plain"));
		R->Code = Code;
		return R;
	}

	/** From this computer, with the token. */
	static bool Allowed(const FHttpServerRequest& Request)
	{
		const FString Peer = Request.PeerAddress ? Request.PeerAddress->ToString(false) : FString();
		if (Peer != TEXT("127.0.0.1") && Peer != TEXT("::1") && Peer != TEXT("::ffff:127.0.0.1")) return false;
		const TArray<FString>* Auth = Request.Headers.Find(TEXT("Authorization"));
		return Auth && Auth->Num() > 0 && (*Auth)[0] == TEXT("Bearer ") + Token;
	}

	static TSharedPtr<FJsonObject> Body(const FHttpServerRequest& Request)
	{
		FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
		TSharedPtr<FJsonObject> Json;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FString(Text.Length(), Text.Get()));
		return FJsonSerializer::Deserialize(Reader, Json) ? Json : nullptr;
	}

	/** A text field, empty when it is not there. */
	static FString Str(const FJsonObject& J, const TCHAR* Key)
	{
		FString V;
		J.TryGetStringField(Key, V);
		return V;
	}

	static ALandscape* Landscape()
	{
		UWorld* W = World();
		if (!W) return nullptr;
		TActorIterator<ALandscape> It(W);
		return It ? *It : nullptr;
	}

	/** A map pixel rectangle as landscape vertices (X1, Y1), or false without a landscape. */
	static bool Corner(const FJsonObject& J, ALandscape*& Land, ULandscapeInfo*& Info, int32& X1, int32& Y1)
	{
		Land = Landscape();
		Info = Land ? Land->GetLandscapeInfo() : nullptr;
		if (!Info) return false;
		const FVector Loc = Land->GetActorLocation(), Scale = Land->GetActorScale3D();
		X1 = FMath::RoundToInt(((J.GetNumberField(TEXT("x0")) + J.GetIntegerField(TEXT("col"))) * 100.0 - Loc.X) / Scale.X);
		Y1 = FMath::RoundToInt(((J.GetNumberField(TEXT("y0")) + J.GetIntegerField(TEXT("row"))) * 100.0 - Loc.Y) / Scale.Y);
		return true;
	}

	static FString Heights(const FJsonObject& J)
	{
		ALandscape* Land;
		ULandscapeInfo* Info;
		int32 X1, Y1;
		const TArray<TSharedPtr<FJsonValue>>* Values;
		if (!Corner(J, Land, Info, X1, Y1) || !J.TryGetArrayField(TEXT("heights"), Values)) return TEXT("no landscape, or no heights");
		const int32 W = J.GetIntegerField(TEXT("w")), H = J.GetIntegerField(TEXT("h"));
		if (W < 1 || H < 1 || Values->Num() != W * H) return TEXT("heights do not match w x h");
		const double ScaleZ = Land->GetActorScale3D().Z;
		TArray<uint16> Data;
		Data.SetNumUninitialized(W * H);
		for (int32 i = 0; i < W * H; i++)
		{
			Data[i] = (uint16)FMath::Clamp(FMath::RoundToInt((*Values)[i]->AsNumber() * 100.0 / ScaleZ * 128.0 + 32768.0), 0, 65535);
		}
		const TArrayView<const FLandscapeLayer> Layers = Land->GetLayersConst();
		const FGuid Layer = Layers.Num() > 0 && Layers[0].EditLayer ? Layers[0].EditLayer->GetGuid() : FGuid();
		{
			FScopedSetLandscapeEditingLayer Scope(Land, Layer, [Land] { Land->RequestLayersContentUpdateForceAll(); });
			FLandscapeEditDataInterface Edit(Info);
			Edit.SetHeightData(X1, Y1, X1 + W - 1, Y1 + H - 1, Data.GetData(), W, true);
			Edit.Flush();
		}
		Land->MarkPackageDirty();
		return FString::Printf(TEXT("heights %d x %d at %d, %d"), W, H, X1, Y1);
	}

	/**
	 * The one texture the landscape material reads whose name starts so, or null: none, or more than one (a name too
	 * vague to say which; nothing is written then).
	 */
	static UTexture2D* LayersTexture(const FString& Prefix)
	{
		ALandscape* Land = Landscape();
		UMaterialInterface* Material = Land ? Land->GetLandscapeMaterial() : nullptr;
		if (!Material || Prefix.IsEmpty()) return nullptr;
		TArray<UTexture*> Used;
		Material->GetUsedTextures(Used);
		UTexture2D* Found = nullptr;
		for (UTexture* T : Used)
		{
			if (UTexture2D* T2 = Cast<UTexture2D>(T); T2 && T2->GetName().StartsWith(Prefix))
			{
				if (Found && Found != T2) return nullptr;
				Found = T2;
			}
		}
		return Found;
	}

	static FString Layers(const FJsonObject& J)
	{
		const int32 Col = J.GetIntegerField(TEXT("col")), Row = J.GetIntegerField(TEXT("row")), W = J.GetIntegerField(TEXT("w")), H = J.GetIntegerField(TEXT("h"));
		const TArray<TSharedPtr<FJsonValue>>* List;
		if (W < 1 || H < 1 || !J.TryGetArrayField(TEXT("layers"), List)) return TEXT("no layers");
		const FString TextureName = Str(J, TEXT("texture"));
		UTexture2D* Tex = LayersTexture(TextureName);
		if (!TextureName.IsEmpty() && !Tex) return TEXT("no layers written: not one texture of the landscape material is named ") + TextureName + TEXT("...");
		uint8* Pixels = nullptr;
		ALandscape* Land;
		ULandscapeInfo* Info;
		int32 X1 = 0, Y1 = 0;
		const bool bLand = Corner(J, Land, Info, X1, Y1);
		TArray<FString> Done, Missing;
		for (const TSharedPtr<FJsonValue>& V : *List)
		{
			const TSharedPtr<FJsonObject>& L = V->AsObject();
			const FString Name = Str(*L, TEXT("name")), Channel = Str(*L, TEXT("channel"));
			TArray<uint8> Weights;
			if (!FBase64::Decode(L->GetStringField(TEXT("weights")), Weights) || Weights.Num() != W * H)
			{
				Missing.Add(Name + TEXT(" (bad weights)"));
				continue;
			}
			// BGRA in the texture's source
			const int32 Offset = Channel == TEXT("r") ? 2 : Channel == TEXT("g") ? 1 : Channel == TEXT("b") ? 0 : Channel == TEXT("a") ? 3 : -1;
			if (Tex && Offset >= 0 && Tex->Source.GetFormat() == TSF_BGRA8)
			{
				const int32 TW = Tex->Source.GetSizeX(), TH = Tex->Source.GetSizeY();
				Pixels = Pixels ? Pixels : Tex->Source.LockMip(0);
				for (int32 y = 0; y < H; y++)
				{
					for (int32 x = 0; x < W; x++)
					{
						const int32 tx = Col + x, ty = Row + y;
						if (tx >= 0 && ty >= 0 && tx < TW && ty < TH) Pixels[(ty * TW + tx) * 4 + Offset] = Weights[y * W + x];
					}
				}
				Done.Add(Name);
				continue;
			}
			const FLandscapeInfoLayerSettings* Paint = bLand ? Info->Layers.FindByPredicate([&](const FLandscapeInfoLayerSettings& S) { return S.LayerInfoObj && S.LayerInfoObj->GetLayerName().ToString() == Name; }) : nullptr;
			if (!Paint)
			{
				Missing.Add(Name);
				continue;
			}
			const TArrayView<const FLandscapeLayer> EditLayers = Land->GetLayersConst();
			const FGuid Layer = EditLayers.Num() > 0 && EditLayers[0].EditLayer ? EditLayers[0].EditLayer->GetGuid() : FGuid();
			{
				FScopedSetLandscapeEditingLayer Scope(Land, Layer, [Land] { Land->RequestLayersContentUpdateForceAll(); });
				FLandscapeEditDataInterface Edit(Info);
				Edit.SetAlphaData(Paint->LayerInfoObj, X1, Y1, X1 + W - 1, Y1 + H - 1, Weights.GetData(), W);
				Edit.Flush();
			}
			Land->MarkPackageDirty();
			Done.Add(Name);
		}
		if (Pixels)
		{
			Tex->Source.UnlockMip(0);
			Tex->PostEditChange();
			Tex->MarkPackageDirty();
		}
		return FString::Printf(TEXT("layers %d x %d: %s"), W, H, *FString::Join(Done, TEXT(", ")))
			+ (Missing.Num() ? TEXT("; not in this level: ") + FString::Join(Missing, TEXT(", ")) : FString());
	}

	/** A marker actor's id (a tag "gw_marker:<id>", or any "<prefix>_marker:<id>"; a PlayerStart is player_start), or empty. */
	static FString MarkerOf(const AActor* A)
	{
		for (const FName& T : A->Tags)
		{
			const FString S = T.ToString();
			if (const int32 At = S.Find(TEXT("marker:")); At == 0 || (At > 0 && S[At - 1] == TEXT('_'))) return S.RightChop(At + 7);
		}
		return A->IsA<APlayerStart>() ? FString(TEXT("player_start")) : FString();
	}

	static FTransform Transform(const FJsonObject& O)
	{
		const FQuat Q(O.GetNumberField(TEXT("qx")), O.GetNumberField(TEXT("qy")), O.GetNumberField(TEXT("qz")), O.GetNumberField(TEXT("qw")));
		return FTransform(Q.GetNormalized(), FVector(O.GetNumberField(TEXT("x")), O.GetNumberField(TEXT("y")), O.GetNumberField(TEXT("z"))),
			FVector(O.GetNumberField(TEXT("sx")), O.GetNumberField(TEXT("sy")), O.GetNumberField(TEXT("sz"))));
	}

	static FString Objects(const FJsonObject& J)
	{
		UWorld* W = World();
		if (!W) return TEXT("no level");
		// every instance component and prop actor of a mesh
		TMap<UStaticMesh*, TArray<UInstancedStaticMeshComponent*>> Instances;
		TMap<UStaticMesh*, TArray<AStaticMeshActor*>> Props;
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			if (AStaticMeshActor* P = Cast<AStaticMeshActor>(*It); P && P->GetStaticMeshComponent() && P->GetStaticMeshComponent()->GetStaticMesh())
			{
				Props.FindOrAdd(P->GetStaticMeshComponent()->GetStaticMesh()).Add(P);
				continue;
			}
			TArray<UInstancedStaticMeshComponent*> Comps;
			It->GetComponents(Comps);
			for (UInstancedStaticMeshComponent* C : Comps)
			{
				if (C->GetStaticMesh()) Instances.FindOrAdd(C->GetStaticMesh()).Add(C);
			}
		}
		int32 NRemoved = 0, NAdded = 0, Missing = 0;
		const TArray<TSharedPtr<FJsonValue>>* List;
		if (J.TryGetArrayField(TEXT("remove"), List))
		{
			for (const TSharedPtr<FJsonValue>& V : *List)
			{
				const TSharedPtr<FJsonObject>& O = V->AsObject();
				const FVector At(O->GetNumberField(TEXT("x")), O->GetNumberField(TEXT("y")), O->GetNumberField(TEXT("z")));
				if (FString Marker; O->TryGetStringField(TEXT("marker"), Marker))
				{
					AActor* Found = nullptr;
					for (TActorIterator<AActor> It(W); It && !Found; ++It)
					{
						if (MarkerOf(*It) == Marker && FVector::DistSquared2D(It->GetActorLocation(), At) < 100.0) Found = *It;
					}
					Found ? (Found->Destroy(), NRemoved++) : Missing++;
					continue;
				}
				UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *O->GetStringField(TEXT("mesh")));
				bool bDone = false;
				for (UInstancedStaticMeshComponent* C : Mesh ? Instances.FindRef(Mesh) : TArray<UInstancedStaticMeshComponent*>())
				{
					for (int32 i = C->GetInstanceCount() - 1; i >= 0 && !bDone; i--)
					{
						FTransform T;
						if (C->GetInstanceTransform(i, T, true) && FVector::DistSquared(T.GetLocation(), At) < 25.0)
						{
							C->Modify();
							C->RemoveInstance(i);
							bDone = true;
						}
					}
					if (bDone) break;
				}
				for (AStaticMeshActor* P : Mesh && !bDone ? Props.FindRef(Mesh) : TArray<AStaticMeshActor*>())
				{
					if (IsValid(P) && FVector::DistSquared(P->GetActorLocation(), At) < 25.0)
					{
						P->Destroy();
						bDone = true;
						break;
					}
				}
				bDone ? NRemoved++ : Missing++;
			}
		}
		if (J.TryGetArrayField(TEXT("add"), List))
		{
			for (const TSharedPtr<FJsonValue>& V : *List)
			{
				const TSharedPtr<FJsonObject>& O = V->AsObject();
				if (FString Marker; O->TryGetStringField(TEXT("marker"), Marker))
				{
					UClass* Class = FindFirstObject<UClass>(*O->GetStringField(TEXT("class")), EFindFirstObjectOptions::NativeFirst);
					const FTransform T = Transform(*O);
					AActor* A = Class && Class->IsChildOf(AActor::StaticClass()) ? W->SpawnActor<AActor>(Class, T.GetLocation(), FRotator(0, T.Rotator().Yaw, 0)) : nullptr;
					if (!A)
					{
						Missing++;
						continue;
					}
					A->Tags.Add(FName(TEXT("gw_marker:") + Marker));
					if (FString Tag; O->TryGetStringField(TEXT("tag"), Tag) && !Tag.IsEmpty()) A->Tags.Add(FName(Tag));
					A->SetActorLabel(TEXT("GW_Marker_") + Marker);
					A->SetFolderPath(TEXT("Groundwork/Markers"));
					NAdded++;
					continue;
				}
				UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *O->GetStringField(TEXT("mesh")));
				if (!Mesh)
				{
					Missing++;
					continue;
				}
				const FTransform T = Transform(*O);
				if (const TArray<UInstancedStaticMeshComponent*>* Comps = Instances.Find(Mesh); Comps && Comps->Num() > 0)
				{
					(*Comps)[0]->Modify();
					(*Comps)[0]->AddInstance(T, true);
				}
				else
				{
					AStaticMeshActor* P = W->SpawnActor<AStaticMeshActor>(T.GetLocation(), T.Rotator());
					P->GetStaticMeshComponent()->SetStaticMesh(Mesh);
					P->SetActorScale3D(T.GetScale3D());
					P->SetActorLabel(TEXT("GW_") + Mesh->GetName());
					P->SetFolderPath(TEXT("Groundwork/Objects"));
					Props.FindOrAdd(Mesh).Add(P);
				}
				NAdded++;
			}
		}
		W->PersistentLevel->MarkPackageDirty();
		return FString::Printf(TEXT("objects: %d added, %d removed, %d not found"), NAdded, NRemoved, Missing);
	}

	static FInterpCurveFloat Curve(const TArray<float>& Values)
	{
		FInterpCurveFloat C;
		for (int32 i = 0; i < Values.Num(); i++)
		{
			C.Points[C.AddPoint(i, Values[i])].InterpMode = CIM_CurveAuto;
		}
		return C;
	}

	/** The rivers and lakes labelled prefix + name, by label (an ocean or anything else is never the map's). */
	static TMap<FString, AWaterBody*> Bodies(UWorld* W, const FString& Prefix)
	{
		TMap<FString, AWaterBody*> Found;
		for (TActorIterator<AWaterBody> It(W); It; ++It)
		{
			if ((It->IsA<AWaterBodyRiver>() || It->IsA<AWaterBodyLake>()) && It->GetActorLabel().StartsWith(Prefix)) Found.Add(It->GetActorLabel(), *It);
		}
		return Found;
	}

	static FString LabelOf(const FString& Prefix, const FString& Name) { return Prefix + Name.Replace(TEXT(" "), TEXT("_")); }

	/** A new water body of this kind, made like the first one already there (materials, falloff...), else Unreal's own. */
	template <typename T>
	static AWaterBody* Spawn(UWorld* W, const FVector& At, const FString& Label)
	{
		FActorSpawnParameters Params;
		TActorIterator<T> Like(W);
		Params.Template = Like ? *Like : nullptr;
		T* Body = W->SpawnActor<T>(At, FRotator::ZeroRotator, Params);
		if (!Body) return nullptr;
		Body->SetActorLabel(Label);
		Body->SetFolderPath(TEXT("Groundwork/Water"));
		return Body;
	}

	static void SetSpline(AWaterBody* Body, const TArray<FVector>& Points, bool bClosed)
	{
		UWaterSplineComponent* S = Body->GetWaterSpline();
		S->Modify();
		S->ClearSplinePoints(false);
		for (const FVector& P : Points) S->AddSplinePoint(P, ESplineCoordinateSpace::World, false);
		S->SetClosedLoop(bClosed, false);
		S->UpdateSpline();
	}

	static FString Water(const FJsonObject& J)
	{
		UWorld* W = World();
		if (!W) return TEXT("no level");
		const FString Prefix = J.HasField(TEXT("prefix")) ? Str(J, TEXT("prefix")) : FString(TEXT("GW_Water_"));
		// the bodies it may rewrite and delete: a real prefix only (letters, digits, _, ending in _), never all water
		if (Prefix.Len() < 3 || !Prefix.EndsWith(TEXT("_")) || Prefix.Contains(TEXT(" "))
			|| !Algo::AllOf(Prefix, [](TCHAR C) { return FChar::IsAlnum(C) || C == TEXT('_'); }))
		{
			return TEXT("no water written: the prefix must be 3 or more letters, digits or _, ending in _");
		}
		TMap<FString, AWaterBody*> Old = Bodies(W, Prefix);
		TSet<FString> Keep;
		int32 Made = 0;
		const TArray<TSharedPtr<FJsonValue>>* List;
		if (J.TryGetArrayField(TEXT("rivers"), List))
		{
			for (const TSharedPtr<FJsonValue>& V : *List)
			{
				const TSharedPtr<FJsonObject>& R = V->AsObject();
				const TArray<TSharedPtr<FJsonValue>>& P = R->GetArrayField(TEXT("points"));
				if (P.Num() < 2) continue;
				const FString Label = LabelOf(Prefix, R->GetStringField(TEXT("name")));
				const double Top = R->GetNumberField(TEXT("top")), End = R->GetNumberField(TEXT("end"));
				// the surface falls from top to end along the river's length
				TArray<double> Along = { 0.0 };
				for (int32 i = 1; i < P.Num(); i++)
				{
					const TArray<TSharedPtr<FJsonValue>>& A = P[i - 1]->AsArray(), & B = P[i]->AsArray();
					Along.Add(Along.Last() + FMath::Sqrt(FMath::Square(B[0]->AsNumber() - A[0]->AsNumber()) + FMath::Square(B[1]->AsNumber() - A[1]->AsNumber())));
				}
				TArray<FVector> Points;
				TArray<float> Width, Depth, Current;
				for (int32 i = 0; i < P.Num(); i++)
				{
					const TArray<TSharedPtr<FJsonValue>>& Q = P[i]->AsArray();
					if (Q.Num() < 5) return TEXT("a river point is not [x, y, width, depth, current]");
					Points.Add(FVector(Q[0]->AsNumber(), Q[1]->AsNumber(), Top + (End - Top) * Along[i] / FMath::Max(Along.Last(), 1e-3)) * 100.0);
					Width.Add(Q[2]->AsNumber() * 100.0);
					Depth.Add(Q[3]->AsNumber() * 100.0);
					Current.Add(Q[4]->AsNumber() * 100.0);
				}
				AWaterBody* Body = Old.FindRef(Label);
				if (!Body && (Body = Spawn<AWaterBodyRiver>(W, Points[0], Label)) != nullptr) Made++;
				if (!Body) continue;
				Keep.Add(Label);
				SetSpline(Body, Points, false);
				if (UWaterSplineMetadata* M = Body->GetWaterSplineMetadata())
				{
					M->Modify();
					M->RiverWidth = Curve(Width);
					M->Depth = Curve(Depth);
					M->WaterVelocityScalar = Curve(Current);
				}
				Body->GetWaterSpline()->K2_SynchronizeAndBroadcastDataChange();
			}
		}
		if (J.TryGetArrayField(TEXT("lakes"), List))
		{
			for (const TSharedPtr<FJsonValue>& V : *List)
			{
				const TSharedPtr<FJsonObject>& L = V->AsObject();
				const TArray<TSharedPtr<FJsonValue>>& O = L->GetArrayField(TEXT("outline"));
				if (O.Num() < 3) continue;
				const FString Label = LabelOf(Prefix, L->GetStringField(TEXT("name")));
				const double Level = L->GetNumberField(TEXT("level"));
				TArray<FVector> Points;
				for (const TSharedPtr<FJsonValue>& Q : O) Points.Add(FVector(Q->AsArray()[0]->AsNumber(), Q->AsArray()[1]->AsNumber(), Level) * 100.0);
				AWaterBody* Body = Old.FindRef(Label);
				if (!Body && (Body = Spawn<AWaterBodyLake>(W, Points[0], Label)) != nullptr) Made++;
				if (!Body) continue;
				Keep.Add(Label);
				SetSpline(Body, Points, true);
				if (UWaterSplineMetadata* M = Body->GetWaterSplineMetadata())
				{
					M->Modify();
					TArray<float> Depth;
					Depth.Init(L->GetNumberField(TEXT("depth")) * 100.0, Points.Num());
					M->Depth = Curve(Depth);
				}
				Body->GetWaterSpline()->K2_SynchronizeAndBroadcastDataChange();
			}
		}
		int32 Removed = 0;
		for (const TPair<FString, AWaterBody*>& B : Old)
		{
			if (!Keep.Contains(B.Key) && B.Value->Destroy()) Removed++;
		}
		W->PersistentLevel->MarkPackageDirty();
		return FString::Printf(TEXT("water: %d bodies, %d new, %d removed"), Keep.Num(), Made, Removed);
	}

	static FString Version(const FJsonObject&)
	{
		return FString::Printf(TEXT("{\"heights\": %lld, \"objects\": %lld, \"water\": %lld}"), HeightsVersion, ObjectsVersion, WaterVersion);
	}

	static FString HeightsAll(const FJsonObject&)
	{
		ALandscape* Land = Landscape();
		ULandscapeInfo* Info = Land ? Land->GetLandscapeInfo() : nullptr;
		int32 X1, Y1, X2, Y2;
		if (!Info || !Info->GetLandscapeExtent(X1, Y1, X2, Y2)) return TEXT("{}");
		const int32 W = X2 - X1 + 1, H = Y2 - Y1 + 1;
		TArray<uint16> Data;
		Data.SetNumZeroed(W * H);
		FLandscapeEditDataInterface Edit(Info);
		Edit.GetHeightDataFast(X1, Y1, X2, Y2, Data.GetData(), W);
		const FVector Loc = Land->GetActorLocation(), Scale = Land->GetActorScale3D();
		return FString::Printf(TEXT("{\"x\": %f, \"y\": %f, \"sx\": %f, \"sy\": %f, \"sz\": %f, \"w\": %d, \"h\": %d, \"data\": \"%s\"}"),
			Loc.X + X1 * Scale.X, Loc.Y + Y1 * Scale.Y, Scale.X, Scale.Y, Scale.Z, W, H, *FBase64::Encode(reinterpret_cast<const uint8*>(Data.GetData()), Data.Num() * 2));
	}

	static FString ObjectsAll(const FJsonObject&)
	{
		UWorld* W = World();
		if (!W) return TEXT("{}");
		FString Out = TEXT("{\"objects\": [");
		bool bFirst = true;
		auto Add = [&](const UStaticMesh* Mesh, const FTransform& T)
		{
			const FVector L = T.GetLocation(), S = T.GetScale3D();
			const FQuat Q = T.GetRotation();
			const FString Path = Mesh->GetPathName().LeftChop(Mesh->GetName().Len() + 1);
			Out += FString::Printf(TEXT("%s[\"%s\",%.1f,%.1f,%.1f,%.6f,%.6f,%.6f,%.6f,%.4f,%.4f,%.4f]"), bFirst ? TEXT("") : TEXT(","), *Path, L.X, L.Y, L.Z, Q.X, Q.Y, Q.Z, Q.W, S.X, S.Y, S.Z);
			bFirst = false;
		};
		FString Markers;
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			AActor* A = *It;
			if (const FString Marker = MarkerOf(A); !Marker.IsEmpty())
			{
				const FVector L = A->GetActorLocation();
				Markers += FString::Printf(TEXT("%s[\"%s\",%.1f,%.1f,%.1f,%.2f]"), Markers.IsEmpty() ? TEXT("") : TEXT(","), *Marker, L.X, L.Y, L.Z, A->GetActorRotation().Yaw);
				continue;
			}
			if (A->IsHiddenEd()) continue;
			// the editor keeps only the meshes it knows (its models' names.json)
			if (AStaticMeshActor* P = Cast<AStaticMeshActor>(A); P && P->GetStaticMeshComponent()->GetStaticMesh() && P->GetStaticMeshComponent()->IsVisible())
			{
				Add(P->GetStaticMeshComponent()->GetStaticMesh(), P->GetActorTransform());
				continue;
			}
			TArray<UInstancedStaticMeshComponent*> Comps;
			A->GetComponents(Comps);
			for (UInstancedStaticMeshComponent* C : Comps)
			{
				if (!C->GetStaticMesh() || !C->IsVisible()) continue;
				for (int32 i = 0; i < C->GetInstanceCount(); i++)
				{
					FTransform T;
					if (C->GetInstanceTransform(i, T, true)) Add(C->GetStaticMesh(), T);
				}
			}
		}
		return Out + TEXT("], \"markers\": [") + Markers + TEXT("]}");
	}

	static FString WaterAll(const FJsonObject&)
	{
		UWorld* W = World();
		if (!W) return TEXT("{}");
		// every river and lake of the level, named by its label (the editor keeps those with its prefix)
		TArray<AWaterBody*> Rivers, Lakes;
		for (TActorIterator<AWaterBody> It(W); It; ++It)
		{
			if (It->IsA<AWaterBodyRiver>()) Rivers.Add(*It);
			else if (It->IsA<AWaterBodyLake>()) Lakes.Add(*It);
		}
		auto ByLabel = [](const AWaterBody& A, const AWaterBody& B) { return A.GetActorLabel() < B.GetActorLabel(); };
		Rivers.Sort(ByLabel);
		Lakes.Sort(ByLabel);
		auto At = [](const FInterpCurveFloat& C, int32 i, float Default) { return C.Points.IsValidIndex(i) ? C.Points[i].OutVal : Default; };
		FString Out = TEXT("{\"rivers\": [");
		for (int32 r = 0; r < Rivers.Num(); r++)
		{
			const UWaterSplineComponent* S = Rivers[r]->GetWaterSpline();
			const UWaterSplineMetadata* M = Rivers[r]->GetWaterSplineMetadata();
			Out += FString::Printf(TEXT("%s{\"name\": \"%s\", \"points\": ["), r ? TEXT(",") : TEXT(""), *Rivers[r]->GetActorLabel());
			for (int32 i = 0; S && i < S->GetNumberOfSplinePoints(); i++)
			{
				const FVector L = S->GetLocationAtSplinePoint(i, ESplineCoordinateSpace::World);
				Out += FString::Printf(TEXT("%s[%.1f,%.1f,%.1f,%.1f,%.1f,%.1f]"), i ? TEXT(",") : TEXT(""), L.X, L.Y, L.Z,
					M ? At(M->RiverWidth, i, 500) : 500.f, M ? At(M->Depth, i, 250) : 250.f, M ? At(M->WaterVelocityScalar, i, 60) : 60.f);
			}
			Out += TEXT("]}");
		}
		Out += TEXT("], \"lakes\": [");
		for (int32 l = 0; l < Lakes.Num(); l++)
		{
			const UWaterSplineComponent* S = Lakes[l]->GetWaterSpline();
			const UWaterSplineMetadata* M = Lakes[l]->GetWaterSplineMetadata();
			Out += FString::Printf(TEXT("%s{\"name\": \"%s\", \"depth\": %.1f, \"points\": ["), l ? TEXT(",") : TEXT(""), *Lakes[l]->GetActorLabel(), M ? At(M->Depth, 0, 300) : 300.f);
			for (int32 i = 0; S && i < S->GetNumberOfSplinePoints(); i++)
			{
				const FVector L = S->GetLocationAtSplinePoint(i, ESplineCoordinateSpace::World);
				Out += FString::Printf(TEXT("%s[%.1f,%.1f,%.1f]"), i ? TEXT(",") : TEXT(""), L.X, L.Y, L.Z);
			}
			Out += TEXT("]}");
		}
		return Out + TEXT("]}");
	}

	/** What a change in Unreal touches: counted per kind unless the editor's own request made it. */
	static void Count(const UObject* O)
	{
		if (bApplying || !O) return;
		const AActor* A = Cast<AActor>(O);
		if (!A)
		{
			if (const UActorComponent* C = Cast<UActorComponent>(O)) A = C->GetOwner();
			else if (const UWaterSplineMetadata* M = Cast<UWaterSplineMetadata>(O)) A = Cast<AActor>(M->GetOuter());
		}
		if (!A) return;
		if (A->IsA<ALandscapeProxy>()) HeightsVersion += FPlatformTime::Seconds() >= QuietUntil;
		else if (A->IsA<AWaterBody>()) WaterVersion++;
		else if (A->IsA<AStaticMeshActor>() || !MarkerOf(A).IsEmpty() || A->FindComponentByClass<UInstancedStaticMeshComponent>()) ObjectsVersion++;
	}

	static void Bind(const TCHAR* Path, EHttpServerRequestVerbs Verb, TFunction<FString(const FJsonObject&)> Apply)
	{
		Routes.Add(Router->BindRoute(FHttpPath(Path), Verb, FHttpRequestHandler::CreateLambda([Apply](const FHttpServerRequest& Request, const FHttpResultCallback& Done)
		{
			if (!Allowed(Request))
			{
				Done(Reply(TEXT("no"), EHttpServerResponseCodes::Denied));
				return true;
			}
			TSharedPtr<FJsonObject> J = Request.Verb == EHttpServerRequestVerbs::VERB_GET ? MakeShared<FJsonObject>() : Body(Request);
			if (!J)
			{
				Done(Reply(TEXT("bad json"), EHttpServerResponseCodes::BadRequest));
				return true;
			}
			bApplying = Request.Verb != EHttpServerRequestVerbs::VERB_GET;
			const FString Result = Apply(*J);
			bApplying = false;
			QuietUntil = Request.Verb != EHttpServerRequestVerbs::VERB_GET ? FPlatformTime::Seconds() + 2.0 : QuietUntil;
			Done(Reply(Result));
			return true;
		})));
	}

	static void Start()
	{
		if (!GIsEditor || IsRunningCommandlet()) return;
		// this port on 127.0.0.1 only, whatever the project's default bind address
		const FString Section = TEXT("HTTPServer.Listeners");
		TArray<FString> Overrides;
		GConfig->GetArray(*Section, TEXT("ListenerOverrides"), Overrides, GEngineIni);
		Overrides.RemoveAll([](const FString& O) { return O.Contains(FString::Printf(TEXT("Port=%u"), Port)); });
		Overrides.Add(FString::Printf(TEXT("(Port=%u,BindAddress=127.0.0.1,ReuseAddressAndPort=true)"), Port));
		GConfig->SetArray(*Section, TEXT("ListenerOverrides"), Overrides, GEngineIni);
		FCoreDelegates::TSOnConfigSectionsChanged().Broadcast(GEngineIni, TSet<FString>{ Section });
		Router = FHttpServerModule::Get().GetHttpRouter(Port);
		if (!Router)
		{
			UE_LOG(LogTemp, Warning, TEXT("Groundwork bridge: port %u is busy"), Port);
			return;
		}
		Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		FFileHelper::SaveStringToFile(FString::Printf(TEXT("{\"port\": %u, \"token\": \"%s\"}"), Port, *Token), *TokenFile());
		Bind(TEXT("/groundwork/ping"), EHttpServerRequestVerbs::VERB_GET, [](const FJsonObject&) { return FString(TEXT("{\"bridge\": \"groundwork\", \"version\": 1}")); });
		Bind(TEXT("/groundwork/heights"), EHttpServerRequestVerbs::VERB_POST, Heights);
		Bind(TEXT("/groundwork/layers"), EHttpServerRequestVerbs::VERB_POST, Layers);
		Bind(TEXT("/groundwork/objects"), EHttpServerRequestVerbs::VERB_POST, Objects);
		Bind(TEXT("/groundwork/water"), EHttpServerRequestVerbs::VERB_POST, Water);
		Bind(TEXT("/groundwork/version"), EHttpServerRequestVerbs::VERB_GET, Version);
		Bind(TEXT("/groundwork/heights_all"), EHttpServerRequestVerbs::VERB_GET, HeightsAll);
		Bind(TEXT("/groundwork/objects_all"), EHttpServerRequestVerbs::VERB_GET, ObjectsAll);
		Bind(TEXT("/groundwork/water_all"), EHttpServerRequestVerbs::VERB_GET, WaterAll);
		Modified = FCoreUObjectDelegates::OnObjectModified.AddLambda([](UObject* O) { Count(O); });
		Moved = GEngine->OnActorMoved().AddLambda([](AActor* A) { Count(A); });
		Added = GEngine->OnLevelActorAdded().AddLambda([](AActor* A) { Count(A); });
		Deleted = GEngine->OnLevelActorDeleted().AddLambda([](AActor* A) { Count(A); });
		Undone = FEditorDelegates::PostUndoRedo.AddLambda([] { if (!bApplying) { HeightsVersion++; ObjectsVersion++; WaterVersion++; } });
		FHttpServerModule::Get().StartAllListeners();
		UE_LOG(LogTemp, Display, TEXT("Groundwork bridge: listening on 127.0.0.1:%u"), Port);
	}

	static void Stop()
	{
		FCoreUObjectDelegates::OnObjectModified.Remove(Modified);
		FEditorDelegates::PostUndoRedo.Remove(Undone);
		if (GEngine)
		{
			GEngine->OnActorMoved().Remove(Moved);
			GEngine->OnLevelActorAdded().Remove(Added);
			GEngine->OnLevelActorDeleted().Remove(Deleted);
		}
		if (Router)
		{
			for (const FHttpRouteHandle& R : Routes) Router->UnbindRoute(R);
		}
		Routes.Empty();
		Router.Reset();
		IFileManager::Get().Delete(*TokenFile());
	}
}

class FGroundworkBridgeModule : public IModuleInterface
{
	virtual void StartupModule() override
	{
		FCoreDelegates::GetOnPostEngineInit().AddLambda([] { GroundworkBridge::Start(); });
	}

	virtual void ShutdownModule() override { GroundworkBridge::Stop(); }
};

IMPLEMENT_MODULE(FGroundworkBridgeModule, GroundworkBridge);
