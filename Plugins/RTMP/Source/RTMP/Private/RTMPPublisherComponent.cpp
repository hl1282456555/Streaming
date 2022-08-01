// Fill out your copyright notice in the Description page of Project Settings.


#include "RTMPPublisherComponent.h"
#include "RTMPPublisher.h"

// Sets default values for this component's properties
URTMPPublisherComponent::URTMPPublisherComponent()
{
	// Set this component to be initialized when the game starts, and to be ticked every frame.  You can turn these features
	// off to improve performance if you don't need them.
	PrimaryComponentTick.bCanEverTick = true;

	// ...
}


// Called when the game starts
void URTMPPublisherComponent::BeginPlay()
{
	Super::BeginPlay();

	// ...
	Publisher = MakeShared<class FRTMPPublisher>();
}


// Called every frame
void URTMPPublisherComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// ...
}

void URTMPPublisherComponent::StartPublish(const FRTMPPublisherConfig& Config)
{
	if (!Publisher || !Publisher->Setup(Config)) {
		return;
	}

	Publisher->StartPublish();
}

void URTMPPublisherComponent::StopPublish()
{
	if (Publisher && Publisher->IsInitialized())
	{
		Publisher->Shutdown();
	}
}

