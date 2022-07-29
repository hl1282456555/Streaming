// Fill out your copyright notice in the Description page of Project Settings.


#include "GameViewportRecorder.h"
#include "Framework/Application/SlateApplication.h"
#include "Slate/SceneViewport.h"
#include "Engine/GameEngine.h"

#if WITH_EDITOR
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "IAssetViewport.h"
#endif

DEFINE_LOG_CATEGORY(LogGameViewportRecorder);

FGameViewportRecorder::FGameViewportRecorder(const FIntPoint& RecordResolution)
{
	bInitialized = SetupBackBufferCapturer(RecordResolution);
	CaptureFrameInterval = std::chrono::milliseconds(0);
	LastFrameTime = std::chrono::steady_clock::now();
}

FGameViewportRecorder::~FGameViewportRecorder()
{
}

FOnViewportRecorded& FGameViewportRecorder::OnViewportRecordedCallback()
{
	return OnViewportRecorded;
}

bool FGameViewportRecorder::IsInitialized() const
{
	return bInitialized;
}

bool FGameViewportRecorder::StartRecord(int32 InCaptureRate)
{
	if (!bInitialized) {
		return false;
	}

	if (OnBackBufferReadyToPresent.IsValid()) {
		UE_LOG(LogGameViewportRecorder, Log, TEXT("Game viewport recorder is already recording."));
		return true;
	}

	if (!FSlateApplication::IsInitialized()) {
		UE_LOG(LogGameViewportRecorder, Warning, TEXT("Slate application is not initialized, can not start record."));
		return false;
	}

	CaptureFrameInterval = std::chrono::milliseconds(1000 / InCaptureRate);

	OnBackBufferReadyToPresent = FSlateApplication::Get().GetRenderer()->OnBackBufferReadyToPresent().AddRaw(this, &FGameViewportRecorder::OnBackBufferReadyToPresentCallback);

	return OnBackBufferReadyToPresent.IsValid();
}

void FGameViewportRecorder::StopRecord()
{
	for (FResolveSurface& Surface : Surfaces)
	{
		// Empty threaded operation.
		Surface.Surface.BlockUntilAvailable();
	}

	FSlateApplication::Get().GetRenderer()->OnBackBufferReadyToPresent().RemoveAll(this);
}

bool FGameViewportRecorder::SetupBackBufferCapturer(FIntPoint Resolution)
{
	TargetSize = Resolution;

	CurrentFrameIndex = 0;
	TargetWindowPtr = nullptr;

	uint32 NumSurfaces = 3;

	TSharedPtr<FSceneViewport> SceneViewport;

#if WITH_EDITOR
	if (GIsEditor)
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::PIE)
			{
				FSlatePlayInEditorInfo* SlatePlayInEditorSession = GEditor->SlatePlayInEditorMap.Find(Context.ContextHandle);
				if (SlatePlayInEditorSession)
				{
					if (SlatePlayInEditorSession->DestinationSlateViewport.IsValid())
					{
						TSharedPtr<IAssetViewport> DestinationLevelViewport = SlatePlayInEditorSession->DestinationSlateViewport.Pin();
						SceneViewport = DestinationLevelViewport->GetSharedActiveViewport();
					}
					else if (SlatePlayInEditorSession->SlatePlayInEditorWindowViewport.IsValid())
					{
						SceneViewport = SlatePlayInEditorSession->SlatePlayInEditorWindowViewport;
					}
				}
			}
		}
	}
	else
#endif
	{
		UGameEngine* gameEngine = Cast<UGameEngine>(GEngine);
		if (gameEngine == nullptr || !gameEngine->SceneViewport.IsValid()) {
			return false;
		}

		SceneViewport = gameEngine->SceneViewport;
	}

	if (!SceneViewport) {
		return false;
	}

	FIntRect CaptureRect(0, 0, SceneViewport->GetSize().X, SceneViewport->GetSize().Y);
	FIntPoint WindowSize(0, 0);

	// Set up the capture rectangle
	TSharedPtr<SViewport> ViewportWidget = SceneViewport->GetViewportWidget().Pin();
	if (!ViewportWidget.IsValid()) {
		return false;
	}

	TSharedPtr<SWindow> Window = FSlateApplication::Get().FindWidgetWindow(ViewportWidget.ToSharedRef());
	if (!Window.IsValid()) {
		return false;
	}

	TargetWindowPtr = Window.Get();
	FGeometry InnerWindowGeometry = Window->GetWindowGeometryInWindow();

	// Find the widget path relative to the window
	FArrangedChildren JustWindow(EVisibility::Visible);
	JustWindow.AddWidget(FArrangedWidget(Window.ToSharedRef(), InnerWindowGeometry));

	FWidgetPath WidgetPath(Window.ToSharedRef(), JustWindow);
	if (!WidgetPath.ExtendPathTo(FWidgetMatcher(ViewportWidget.ToSharedRef()), EVisibility::Visible)) {
		return false;
	}

	FArrangedWidget ArrangedWidget = WidgetPath.FindArrangedWidget(ViewportWidget.ToSharedRef()).Get(FArrangedWidget::GetNullWidget());

	FVector2D Position = ArrangedWidget.Geometry.GetAbsolutePosition();
	FVector2D Size = ArrangedWidget.Geometry.GetAbsoluteSize();

	CaptureRect = FIntRect(
		Position.X,
		Position.Y,
		Position.X + Size.X,
		Position.Y + Size.Y);

	FVector2D AbsoluteSize = InnerWindowGeometry.GetAbsoluteSize();
	WindowSize = FIntPoint(AbsoluteSize.X, AbsoluteSize.Y);

	// This can never be reallocated
	Surfaces.Reserve(NumSurfaces);
	for (uint32 Index = 0; Index < NumSurfaces; ++Index)
	{
		Surfaces.Emplace(EPixelFormat::PF_B8G8R8A8, Resolution);
		Surfaces.Last().Surface.SetCaptureRect(CaptureRect);
		Surfaces.Last().Surface.SetWindowSize(WindowSize);
	}

	FrameGrabLatency = 0;

	// Ensure textures are setup
	FlushRenderingCommands();
	return true;
}

void FGameViewportRecorder::OnBackBufferReadyToPresentCallback(SWindow& SlateWindow, const FTexture2DRHIRef& BackBuffer)
{
	// We only care about our own Slate window
	if (&SlateWindow != TargetWindowPtr)
	{
		return;
	}

	check(IsInRenderingThread());

	std::chrono::steady_clock::time_point NowTime = std::chrono::steady_clock::now();
	std::chrono::milliseconds LastFramePassedTime = std::chrono::duration_cast<std::chrono::milliseconds>(NowTime - LastFrameTime);
	if (LastFramePassedTime < CaptureFrameInterval)
	{
		return;
	}

	LastFrameTime = NowTime;

	const int32 PrevCaptureIndexOffset = FMath::Clamp(FrameGrabLatency, 0, Surfaces.Num() - 1);
	const int32 ThisCaptureIndex = CurrentFrameIndex;
	const int32 PrevCaptureIndex = (CurrentFrameIndex - PrevCaptureIndexOffset) < 0 ? Surfaces.Num() - (PrevCaptureIndexOffset - CurrentFrameIndex) : (CurrentFrameIndex - PrevCaptureIndexOffset);

	FResolveSurface* NextFrameTarget = &Surfaces[ThisCaptureIndex];
	NextFrameTarget->Surface.BlockUntilAvailable();

	NextFrameTarget->Surface.Initialize();

	FViewportSurfaceReader* PrevFrameTarget = &Surfaces[PrevCaptureIndex].Surface;

	//If the latency is 0, then we are asking to readback the frame we are currently queuing immediately. 
	if (!PrevFrameTarget->WasEverQueued() && (PrevCaptureIndexOffset > 0))
	{
		PrevFrameTarget = nullptr;
	}

	Surfaces[ThisCaptureIndex].Surface.ResolveRenderTarget(PrevFrameTarget, BackBuffer, [=](FColor* ColorBuffer, int32 Width, int32 Height) {
		// Handle the frame
		OnFrameReady(ThisCaptureIndex, ColorBuffer, Width, Height);
		});

	CurrentFrameIndex = (CurrentFrameIndex + 1) % Surfaces.Num();
}

void FGameViewportRecorder::OnFrameReady(int32 SurfaceIndex, FColor* ColorBuffer, int32 Width, int32 Height)
{
	OnViewportRecorded.Broadcast(ColorBuffer, Width, Height);
}
