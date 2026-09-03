#include "AvidScriptPrimitiveHost.h"

#include "Components/StaticMeshComponent.h"
#include "UObject/ConstructorHelpers.h"

AAvidScriptPrimitiveHost::AAvidScriptPrimitiveHost()
{
	PrimaryActorTick.bCanEverTick = false;

	PrimitiveMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PrimitiveMesh"));
	SetRootComponent(PrimitiveMesh);
	PrimitiveMesh->SetMobility(EComponentMobility::Movable);
	PrimitiveMesh->SetGenerateOverlapEvents(true);
	PrimitiveMesh->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	PrimitiveMesh->SetCollisionObjectType(ECC_WorldDynamic);
	PrimitiveMesh->SetCollisionResponseToAllChannels(ECR_Ignore);
	PrimitiveMesh->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	PrimitiveMesh->SetRelativeScale3D(FVector(0.6));

	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(
		TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (SphereMesh.Succeeded())
	{
		PrimitiveMesh->SetStaticMesh(SphereMesh.Object);
	}
}
