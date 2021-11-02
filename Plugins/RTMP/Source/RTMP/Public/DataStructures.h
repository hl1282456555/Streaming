// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "DataStructures.generated.h"

struct FEncodeFramePayload
{
	TArray<uint8> Data;
	uint32 Width;
	uint32 Height;
};


USTRUCT(BlueprintType)
struct FRTMPPublisherConfig
{
	GENERATED_BODY()
public:
	// Publisher config
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTMP | Config")
	FString StreamUrl;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTMP | Config")
	FString StreamKey;

	// Video config
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTMP | Config")
	int32 Width;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTMP | Config")
	int32 Height;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTMP | Config")
	int32 Framerate;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTMP | Config")
	int32 VideoBitrate;

	// Audio config
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTMP | Config")
	int32 ChannelCount;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTMP | Config")
	int32 SampleRate;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTMP | Config")
	int32 AudioBitrate;
};