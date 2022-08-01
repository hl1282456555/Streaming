// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "AudioDevice.h"
#include "DataStructures.h"

DECLARE_LOG_CATEGORY_EXTERN(LogRTMPPublisher, Log, All);
DECLARE_LOG_CATEGORY_EXTERN(LogFFMPEGEncoder_Video, Log, All);
DECLARE_LOG_CATEGORY_EXTERN(LogFFMPEGEncoder_Audio, Log, All);

struct FOutputStream
{
	struct AVStream* Stream = nullptr;
	struct AVCodecContext* CodecCtx = nullptr;

	int64 NextPts = 0;
	int32 SamplesCount = 0;

	struct AVFrame* Frame = nullptr;
	struct AVFrame* TempFrame = nullptr;

	struct SwsContext* SwsCtx = nullptr;
	struct SwrContext* SwrCtx = nullptr;
};

/**
 * 
 */
class RTMP_API FRTMPPublisher : public FRunnable, public ISubmixBufferListener
{
public:
	FRTMPPublisher();
	~FRTMPPublisher();

	// FRunnable interface imp
	virtual bool Init() override;
	virtual uint32 Run();
	virtual void Stop() override;

	virtual void OnNewSubmixBuffer(const USoundSubmix* OwningSubmix, float* AudioData, int32 NumSamples, int32 NumChannels, const int32 SampleRate, double AudioClock);

	bool Setup(const FRTMPPublisherConfig& Config);

	bool StartPublish();

	void Shutdown();

	bool IsInitialized() const;

protected:

	bool AddStream(FOutputStream& Stream, struct AVCodec** Codec, enum AVCodecID CodecId);
	
	bool OpenVideoStream();
	bool OpenAudioStream();

	struct AVFrame* AllocPicture(enum AVPixelFormat Format, int32 Width, int32 Height);
	struct AVFrame* AllocAudioFrame(enum AVSampleFormat Format, uint64 ChannelLayout, int32 SampleRate, int32 SamplesCount);

	void CloseStream(FOutputStream& Stream);

	bool SendVideoFrame();
	bool SendAudioFrame();

	bool SendFrameInternal(const struct AVRational* TimeBase, struct AVStream* Stream, struct AVPacket* Packet);

	void OnViewportRecorded(const FColor* ColorBuffer, uint32 Width, uint32 Height);

private:
	bool bInitialized;
	bool bHeaderSent;
	FDateTime StartRecordTime;

	FRTMPPublisherConfig PublisherConfig;

	struct AVOutputFormat* OutputFormat;
	struct AVFormatContext* OutputFormatCtx;
	struct AVCodec* VideoCodec;
	struct AVCodec* AudioCodec;

	FOutputStream VideoStream;
	FOutputStream AudioStream;

	TSharedPtr<class FGameViewportRecorder> ViewportRecorder;

	bool bStopEncodeThread;
	FRunnableThread* EncodeThread;

	FCriticalSection VideoFrameQueueCS;
	TQueue<FEncodeFramePayload> VideoFrameQueue;
	FEncodeFramePayload FrozenFrame;

	FCriticalSection AudioSubmixBufferCS;
	TArray<uint8> AudioSubmixBuffer;
};
