// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "FrameGrabber.h"

#include <chrono>

DECLARE_LOG_CATEGORY_EXTERN(LogGameViewportRecorder, Log, All);
DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnViewportRecorded, const FColor*, uint32, uint32);

/**
 * 
 */
class RTMP_API FGameViewportRecorder
{
public:
	FGameViewportRecorder(const FIntPoint& RecordResolution);
	~FGameViewportRecorder();

	FOnViewportRecorded& OnViewportRecordedCallback();

	bool IsInitialized() const;

	bool StartRecord(int32 InCaptureRate);
	void StopRecord();

protected:
	bool SetupBackBufferCapturer(FIntPoint Resolution);

	/** Callback for when a backbuffer is ready for reading (called on render thread) */
	void OnBackBufferReadyToPresentCallback(SWindow& SlateWindow, const FTexture2DRHIRef& BackBuffer);

	/** Called when the specified surface index has been locked for reading with the render target data (called on render thread)  */
	void OnFrameReady(int32 SurfaceIndex, FColor* ColorBuffer, int32 Width, int32 Height);

private:
	bool bInitialized;

	// framerate i.e:33ms = 30fps
	std::chrono::milliseconds CaptureFrameInterval;
	std::chrono::steady_clock::time_point LastFrameTime;

	FOnViewportRecorded OnViewportRecorded;

	/**
	* Pointer to the window we want to capture.
	* Only held for comparison inside OnBackBufferReadyToPresentCallback - never to be dereferenced or cast to an SWindow.
	* Held as a raw pointer to ensure that no referenc counting occurs from the background thread in OnBackBufferReadyToPresentCallback.
	*/
	void* TargetWindowPtr;

	/** Delegate handle for the OnBackBufferReadyToPresent event */
	FDelegateHandle OnBackBufferReadyToPresent;

	/** Array of surfaces that we resolve the viewport RHI to. Fixed allocation - should never be resized */
	struct FResolveSurface
	{
		FResolveSurface(EPixelFormat InPixelFormat, FIntPoint BufferSize) : Surface(InPixelFormat, BufferSize) {}

		FFramePayloadPtr Payload;
		FViewportSurfaceReader Surface;
	};
	TArray<FResolveSurface> Surfaces;

	/** Index into the above array to the next surface that we should use - only accessed on main thread */
	int32 CurrentFrameIndex;

	int32 FrameGrabLatency;

	/** The desired target size to resolve frames to */
	FIntPoint TargetSize;
};
