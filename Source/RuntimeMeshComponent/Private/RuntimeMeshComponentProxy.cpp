// Copyright 2016-2018 Chris Conway (Koderz). All Rights Reserved.

#include "RuntimeMeshComponentPlugin.h"
#include "RuntimeMeshComponent.h"
#include "RuntimeMeshComponentProxy.h"

FRuntimeMeshSceneProxy::FRuntimeMeshSceneProxy(URuntimeMeshComponent* Component) 
	: FPrimitiveSceneProxy(Component)
	, BodySetup(Component->GetBodySetup())
{
	bStaticElementsAlwaysUseProxyPrimitiveUniformBuffer = true;

	check(Component->GetRuntimeMesh() != nullptr);

	RuntimeMesh = Component->GetRuntimeMesh()->EnsureProxyCreated();

	// Setup our material map


	for (auto SectionId : Component->GetRuntimeMesh()->GetSectionIds())
	{
		UMaterialInterface* Mat = Component->GetMaterial(SectionId);
		if (Mat == nullptr)
		{
			Mat = UMaterial::GetDefaultMaterial(MD_Surface);
		}

		SectionRenderData.Add(SectionId, FRuntimeMeshSectionRenderData{ Mat, false });

		MaterialRelevance |= Mat->GetRelevance(GetScene().GetFeatureLevel());
	}
}

FRuntimeMeshSceneProxy::~FRuntimeMeshSceneProxy()
{

}

void FRuntimeMeshSceneProxy::CreateRenderThreadResources()
{
	RuntimeMesh->CalculateViewRelevance(bHasStaticSections, bHasDynamicSections, bHasShadowableSections);

	for (auto& Entry : SectionRenderData)
	{
		if (RuntimeMesh->GetSections().Contains(Entry.Key))
		{
			FRuntimeMeshSectionProxyPtr Section = RuntimeMesh->GetSections()[Entry.Key];

			auto& RenderData = SectionRenderData[Entry.Key];

			RenderData.bWantsAdjacencyInfo = RequiresAdjacencyInformation(RenderData.Material, RuntimeMesh->GetSections()[Entry.Key]->GetVertexFactory()->GetType(), GetScene().GetFeatureLevel());
		}
	}

	FPrimitiveSceneProxy::CreateRenderThreadResources();
}

FPrimitiveViewRelevance FRuntimeMeshSceneProxy::GetViewRelevance(const FSceneView* View) const
{
	FPrimitiveViewRelevance Result;
	Result.bDrawRelevance = IsShown(View);
	Result.bShadowRelevance = IsShadowCast(View);

	bool bForceDynamicPath = !IsStaticPathAvailable() || IsRichView(*View->Family) || IsSelected() || View->Family->EngineShowFlags.Wireframe;
	Result.bStaticRelevance = !bForceDynamicPath && bHasStaticSections;
	Result.bDynamicRelevance = bForceDynamicPath || bHasDynamicSections;

	Result.bRenderInMainPass = ShouldRenderInMainPass();
	Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
	Result.bRenderCustomDepth = ShouldRenderCustomDepth();
	MaterialRelevance.SetPrimitiveViewRelevance(Result);
	return Result;
}

void FRuntimeMeshSceneProxy::CreateMeshBatch(FMeshBatch& MeshBatch, const FRuntimeMeshSectionProxyPtr& Section, const FRuntimeMeshSectionRenderData& RenderData, FMaterialRenderProxy* Material, FMaterialRenderProxy* WireframeMaterial) const
{
	bool bRenderWireframe = WireframeMaterial != nullptr;
	bool bWantsAdjacency = !bRenderWireframe && RenderData.bWantsAdjacencyInfo;

	Section->CreateMeshBatch(MeshBatch, bWantsAdjacency);
	MeshBatch.bWireframe = WireframeMaterial != nullptr;
	MeshBatch.MaterialRenderProxy = MeshBatch.bWireframe ? WireframeMaterial : Material;

	MeshBatch.ReverseCulling = IsLocalToWorldDeterminantNegative();
	MeshBatch.bCanApplyViewModeOverrides = true;

	FMeshBatchElement& BatchElement = MeshBatch.Elements[0];
	BatchElement.PrimitiveUniformBufferResource = &GetUniformBuffer();
}

void FRuntimeMeshSceneProxy::DrawStaticElements(FStaticPrimitiveDrawInterface* PDI)
{
	for (const auto& SectionEntry : RuntimeMesh->GetSections())
	{
		FRuntimeMeshSectionProxyPtr Section = SectionEntry.Value;
		if (Section.IsValid() && Section->ShouldRender() && Section->WantsToRenderInStaticPath())
		{
			const FRuntimeMeshSectionRenderData& RenderData = SectionRenderData[SectionEntry.Key];
			FMaterialRenderProxy* Material = RenderData.Material->GetRenderProxy(false);

			FMeshBatch MeshBatch;
			CreateMeshBatch(MeshBatch, Section, RenderData, Material, nullptr);
			PDI->DrawMesh(MeshBatch, FLT_MAX);
		}
	}
}

void FRuntimeMeshSceneProxy::GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const
{
	// Set up wireframe material (if needed)
	const bool bWireframe = AllowDebugViewmodes() && ViewFamily.EngineShowFlags.Wireframe;

	FColoredMaterialRenderProxy* WireframeMaterialInstance = nullptr;
	if (bWireframe)
	{
		WireframeMaterialInstance = new FColoredMaterialRenderProxy(
			GEngine->WireframeMaterial ? GEngine->WireframeMaterial->GetRenderProxy(IsSelected()) : nullptr,
			FLinearColor(0, 0.5f, 1.f)
		);

		Collector.RegisterOneFrameMaterialProxy(WireframeMaterialInstance);
	}

	// Iterate over sections
	for (const auto& SectionEntry : RuntimeMesh->GetSections())
	{
		FRuntimeMeshSectionProxyPtr Section = SectionEntry.Value;
		if (Section.IsValid() && Section->ShouldRender())
		{
			// Add the mesh batch to every view it's visible in
			for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
			{
				if (VisibilityMap & (1 << ViewIndex))
				{
					bool bForceDynamicPath = IsRichView(*Views[ViewIndex]->Family) || Views[ViewIndex]->Family->EngineShowFlags.Wireframe || IsSelected() || !IsStaticPathAvailable();

					if (bForceDynamicPath || !Section->WantsToRenderInStaticPath())
					{
						const FRuntimeMeshSectionRenderData& RenderData = SectionRenderData[SectionEntry.Key];
						FMaterialRenderProxy* Material = RenderData.Material->GetRenderProxy(IsSelected());

						FMeshBatch& MeshBatch = Collector.AllocateMesh();
						CreateMeshBatch(MeshBatch, Section, RenderData, Material, WireframeMaterialInstance);

						Collector.AddMesh(ViewIndex, MeshBatch);
					}
				}
			}
		}
	}

	// Draw bounds
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		if (VisibilityMap & (1 << ViewIndex))
		{
			// Draw simple collision as wireframe if 'show collision', and collision is enabled, and we are not using the complex as the simple
			if (ViewFamily.EngineShowFlags.Collision && IsCollisionEnabled() && BodySetup && BodySetup->GetCollisionTraceFlag() != ECollisionTraceFlag::CTF_UseComplexAsSimple)
			{
				FTransform GeomTransform(GetLocalToWorld());
				BodySetup->AggGeom.GetAggGeom(GeomTransform, GetSelectionColor(FColor(157, 149, 223, 255), IsSelected(), IsHovered()).ToFColor(true), NULL, false, false, UseEditorDepthTest(), ViewIndex, Collector);
			}

			// Render bounds
			RenderBounds(Collector.GetPDI(ViewIndex), ViewFamily.EngineShowFlags, GetBounds(), IsSelected());
		}
	}
#endif
}