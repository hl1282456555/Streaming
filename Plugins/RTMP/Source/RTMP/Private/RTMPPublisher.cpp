// Fill out your copyright notice in the Description page of Project Settings.


#include "RTMPPublisher.h"
#include "Misc/ScopeExit.h"
#include "GameViewportRecorder.h"

THIRD_PARTY_INCLUDES_START
extern "C" {
#include <libavutil/avassert.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavcodec/codec.h>
#include <libavutil/imgutils.h>
#include <libavutil/error.h>
}
THIRD_PARTY_INCLUDES_END

DEFINE_LOG_CATEGORY(LogRTMPPublisher);
DEFINE_LOG_CATEGORY(LogFFMPEGEncoder_Video);
DEFINE_LOG_CATEGORY(LogFFMPEGEncoder_Audio);

FRTMPPublisher::FRTMPPublisher()
	: bInitialized(false)
	, bHeaderSent(false)
	, CapturedVideoFrameCount(0)
	, ViewportRecorder(nullptr)
	, EncodeThread(nullptr)
{
	av_register_all();
	avformat_network_init();
}

FRTMPPublisher::~FRTMPPublisher()
{
	avformat_network_deinit();
}

bool FRTMPPublisher::Init()
{
	if (!bInitialized) {
		return false;
	}

	bStopEncodeThread = false;
	return true;
}

uint32 FRTMPPublisher::Run()
{
	while (!bStopEncodeThread)
	{
		bool bSentFrameSuccess = true;
		if (av_compare_ts(VideoStream.NextPts, VideoStream.CodecCtx->time_base, AudioStream.NextPts, AudioStream.CodecCtx->time_base) <= 0) {
			bSentFrameSuccess = SendVideoFrame();
		}
		else {
			bSentFrameSuccess = SendAudioFrame();
		}

		if (!bSentFrameSuccess) {
			FPlatformProcess::Sleep(0.01);
		}
	}

	return 0;
}

void FRTMPPublisher::Stop()
{
	bStopEncodeThread = true;
}

void FRTMPPublisher::OnNewSubmixBuffer(const USoundSubmix* OwningSubmix, float* AudioData, int32 NumSamples, int32 NumChannels, const int32 SampleRate, double AudioClock)
{
	Audio::AlignedFloatBuffer InData;
	InData.Append(AudioData, NumSamples);
	Audio::TSampleBuffer<float> FloatBuffer(InData, NumChannels, SampleRate);

	if (FloatBuffer.GetNumChannels() != 2)
	{
		FloatBuffer.MixBufferToChannels(2);
	}
	FloatBuffer.Clamp();

	Audio::TSampleBuffer<int16> PCMData;
	PCMData = FloatBuffer;

	{
		FScopeLock Lock(&AudioSubmixBufferCS);
		AudioSubmixBuffer.Append(reinterpret_cast<const uint8*>(PCMData.GetData()), PCMData.GetNumSamples() * sizeof(*PCMData.GetData()));
	}
}

bool FRTMPPublisher::Setup(const FRTMPPublisherConfig& Config)
{
	if (bInitialized) {
		UE_LOG(LogRTMPPublisher, Warning, TEXT("Publisher is already running."));
		return false;
	}

	PublisherConfig = Config;

	ViewportRecorder = MakeShared<class FGameViewportRecorder>(FIntPoint(PublisherConfig.Width, PublisherConfig.Height));

	ViewportRecorder->OnViewportRecordedCallback().AddRaw(this, &FRTMPPublisher::OnViewportRecorded);

	FString CombinedUrl = Config.StreamUrl;

	if (!Config.StreamKey.IsEmpty()) {
		CombinedUrl += TEXT(" ") + Config.StreamKey;
	}
	
	int32 Result = avformat_alloc_output_context2(&OutputFormatCtx, nullptr, "flv", TCHAR_TO_ANSI(*CombinedUrl));
	if (Result < 0) {
		UE_LOG(LogRTMPPublisher, Error, TEXT("Cloud not allocate output format context."));
		return false;
	}

	OutputFormat = OutputFormatCtx->oformat;

	if (!AddStream(VideoStream, &VideoCodec, AV_CODEC_ID_H264) || !AddStream(AudioStream, &AudioCodec, AV_CODEC_ID_AAC)) {
		Shutdown();
		return false;
	}

	if (!OpenVideoStream() || !OpenAudioStream()) {
		Shutdown();
		return false;
	}

	bInitialized = true;

	av_dump_format(OutputFormatCtx, 0, TCHAR_TO_ANSI(*CombinedUrl), 1);
	
	return true;
}

bool FRTMPPublisher::StartPublish()
{
	if (!bInitialized) {
		UE_LOG(LogRTMPPublisher, Log, TEXT("RTMP publisher is already running."));
		return true;
	}

	FString CombinedUrl = PublisherConfig.StreamUrl;
	if (!PublisherConfig.StreamKey.IsEmpty()) {
		CombinedUrl += TEXT(" ") + PublisherConfig.StreamKey;
	}

	if (OutputFormat->flags & AVFMT_NOFILE) {
		UE_LOG(LogRTMPPublisher, Error, TEXT("Output format flags has AVFMT_NOFILE."));
		return false;
	}


	int32 Result = avio_open(&OutputFormatCtx->pb, TCHAR_TO_ANSI(*CombinedUrl), AVIO_FLAG_WRITE);
	if (Result < 0) {
		UE_LOG(LogRTMPPublisher, Error, TEXT("Cloud not open output file."));
		return false;
	}

	Result = avformat_write_header(OutputFormatCtx, nullptr);
	if (Result < 0) {
		UE_LOG(LogRTMPPublisher, Error, TEXT("Error occurred when opening output file."));
		return false;
	}

	bHeaderSent = true;

	StartTime = FDateTime::Now();

	FAudioDevice* AudioDevice = GEngine->GetMainAudioDeviceRaw();
	if (AudioDevice) {
		AudioDevice->RegisterSubmixBufferListener(this);
	}

	if (!ViewportRecorder->StartRecord()) {
		UE_LOG(LogRTMPPublisher, Error, TEXT("Cloud not start to record game viewport."));
		return false;
	}

	EncodeThread = FRunnableThread::Create(this, TEXT("RTMP Publisher"));
	if (EncodeThread == nullptr) {
		UE_LOG(LogRTMPPublisher, Error, TEXT("Cloud not create new thread to publish."));
		return false;
	}

	return true;
}

void FRTMPPublisher::Shutdown()
{
	// Clear viewport recorder
	if (ViewportRecorder) {
		ViewportRecorder->StopRecord();
		ViewportRecorder.Reset();
	}

	FAudioDevice* AudioDevice = GEngine->GetMainAudioDeviceRaw();
	if (AudioDevice)
	{
		AudioDevice->UnregisterSubmixBufferListener(this);
	}

	// Stop encode thread
	if (EncodeThread != nullptr) {
		EncodeThread->Kill(true);
		EncodeThread->WaitForCompletion();
		EncodeThread = nullptr;
	}

	if (bHeaderSent) {
		av_write_trailer(OutputFormatCtx);
	}

	if (VideoStream.Stream	 != nullptr) {
		CloseStream(VideoStream);
	}

	if (AudioStream.Stream != nullptr) {
		CloseStream(AudioStream);
	}

	if (!(OutputFormat->flags & AVFMT_NOFILE)) {
		avio_closep(&OutputFormatCtx->pb);
	}

	avformat_free_context(OutputFormatCtx);

	// Clear publisher status
	bInitialized = false;
	bHeaderSent = false;
	StartTime = 0;

	VideoFrameQueue.Empty();
	AudioSubmixBuffer.Empty();
}

bool FRTMPPublisher::AddStream(FOutputStream& Stream, struct AVCodec** Codec, enum AVCodecID CodecId)
{
	AVCodecContext* CodecCtx;
	if (CodecId == AV_CODEC_ID_H264) {
		*Codec = avcodec_find_encoder_by_name("h264_nvenc");
		if (*Codec == nullptr) {
			*Codec = avcodec_find_encoder(CodecId);
		}
	}
	else {
		*Codec = avcodec_find_encoder(CodecId);
	}
	if ((*Codec) == nullptr) {
		UE_LOG(LogRTMPPublisher, Error, TEXT("Cloud not find encoder for '%s'"), avcodec_get_name(CodecId));
		return false;
	}

	Stream.Stream = avformat_new_stream(OutputFormatCtx, nullptr);
	if (Stream.Stream == nullptr) {
		UE_LOG(LogRTMPPublisher, Error, TEXT("Cloud not allocate stream."));
		return false;
	}

	Stream.Stream->id = OutputFormatCtx->nb_streams - 1;
	CodecCtx = avcodec_alloc_context3(*Codec);
	if (CodecCtx == nullptr) {
		UE_LOG(LogRTMPPublisher, Error, TEXT("Cloud not alloc an encoding context."));
		return false;
	}

	Stream.CodecCtx = CodecCtx;

	switch ((*Codec)->type)
	{
	case AVMEDIA_TYPE_AUDIO:
	{
		CodecCtx->sample_fmt =AV_SAMPLE_FMT_FLTP;
		CodecCtx->bit_rate = PublisherConfig.AudioBitrate;
		CodecCtx->sample_rate = PublisherConfig.SampleRate;
		CodecCtx->channel_layout = AV_CH_LAYOUT_STEREO;
		CodecCtx->channels = av_get_channel_layout_nb_channels(CodecCtx->channel_layout);
		Stream.Stream->time_base = { 1, CodecCtx->sample_rate };
		break;
	}
	case AVMEDIA_TYPE_VIDEO:
	{
		CodecCtx->bit_rate = PublisherConfig.VideoBitrate;
		CodecCtx->rc_min_rate = CodecCtx->bit_rate;
		CodecCtx->rc_max_rate = CodecCtx->bit_rate;
		CodecCtx->bit_rate_tolerance = CodecCtx->bit_rate;
		CodecCtx->rc_buffer_size = CodecCtx->bit_rate;
		CodecCtx->width = PublisherConfig.Width;
		CodecCtx->height = PublisherConfig.Height;
		
		Stream.Stream->time_base = { 1, PublisherConfig.Framerate };
		CodecCtx->time_base = Stream.Stream->time_base;
		CodecCtx->framerate = { PublisherConfig.Framerate, 1 };
		Stream.Stream->avg_frame_rate = CodecCtx->framerate;
		CodecCtx->frame_number = 1;

		CodecCtx->gop_size = PublisherConfig.Framerate;
		CodecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
		CodecCtx->profile = FF_PROFILE_H264_BASELINE;
		if (CodecCtx->codec_id == AV_CODEC_ID_MPEG2VIDEO) {
			CodecCtx->max_b_frames = 2;
		}

		if (CodecCtx->codec_id == AV_CODEC_ID_MPEG1VIDEO) {
			CodecCtx->mb_decision = 2;
		}
		av_opt_set(CodecCtx->priv_data, "preset", "fast", 0);
		av_opt_set(CodecCtx->priv_data, "profile", "baseline", 0);

		break;
	}
	default:
		return false;
	}

	if (OutputFormatCtx->oformat->flags & AVFMT_GLOBALHEADER) {
		CodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}

	CodecCtx->codec_tag = 0;
	Stream.Stream->codecpar->codec_tag = 0;

	return true;
}

bool FRTMPPublisher::OpenVideoStream()
{
	AVCodecContext* CodecCtx = VideoStream.CodecCtx;

	if (avcodec_open2(CodecCtx, VideoCodec, nullptr) < 0) {
		UE_LOG(LogRTMPPublisher, Error, TEXT("Cloud not open video codec."));
		return false;
	}

	VideoStream.Frame = AllocPicture(CodecCtx->pix_fmt, CodecCtx->width, CodecCtx->height);
	if (VideoStream.Frame == nullptr) {
		UE_LOG(LogRTMPPublisher, Error, TEXT("Cloud not allocate video frame."));
		return false;
	}

	VideoStream.TempFrame = AllocPicture(AV_PIX_FMT_BGRA, CodecCtx->width, CodecCtx->height);
	if (VideoStream.TempFrame == nullptr) {
		UE_LOG(LogRTMPPublisher, Error, TEXT("Cloud not allocate temporary picture."));
		return false;
	}

	if (avcodec_parameters_from_context(VideoStream.Stream->codecpar, CodecCtx) < 0) {
		UE_LOG(LogRTMPPublisher, Error, TEXT("Cloud not copy the stream parameters."));
		return false;
	}

	return true;
}

bool FRTMPPublisher::OpenAudioStream()
{
	AVCodecContext* CodecCtx = AudioStream.CodecCtx;

	if (avcodec_open2(CodecCtx, AudioCodec, nullptr) < 0) {
		UE_LOG(LogRTMPPublisher, Error, TEXT("Cloud not open audio codec."));
		return false;
	}

	int32 SamplesCount = 0;
	if (CodecCtx->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE) {
		SamplesCount = 10000;
	}
	else {
		SamplesCount = CodecCtx->frame_size;
	}

	AudioStream.Frame = AllocAudioFrame(CodecCtx->sample_fmt, CodecCtx->channel_layout, CodecCtx->sample_rate, SamplesCount);
	AudioStream.TempFrame = AllocAudioFrame(AV_SAMPLE_FMT_S16, CodecCtx->channel_layout, CodecCtx->sample_rate, SamplesCount);

	if (avcodec_parameters_from_context(AudioStream.Stream->codecpar, CodecCtx) < 0) {
		UE_LOG(LogRTMPPublisher, Error, TEXT("Cloud not copy the stream parameters."));
		return false;
	}

	AudioStream.SwrCtx = swr_alloc();
	if (AudioStream.SwrCtx == nullptr) {
		UE_LOG(LogRTMPPublisher, Error, TEXT("Cloud not allocate resampler context."));
		return false;
	}

	av_opt_set_int(AudioStream.SwrCtx, "in_channel_count", CodecCtx->channels, 0);
	av_opt_set_int(AudioStream.SwrCtx, "in_sample_rate", CodecCtx->sample_rate, 0);
	av_opt_set_int(AudioStream.SwrCtx, "in_sample_fmt", AV_SAMPLE_FMT_S16, 0);
	av_opt_set_int(AudioStream.SwrCtx, "out_channel_count", CodecCtx->channels, 0);
	av_opt_set_int(AudioStream.SwrCtx, "out_sample_rate", CodecCtx->sample_rate, 0);
	av_opt_set_int(AudioStream.SwrCtx, "out_sample_fmt", CodecCtx->sample_fmt, 0);

	if (swr_init(AudioStream.SwrCtx) < 0) {
		UE_LOG(LogRTMPPublisher, Error, TEXT("Failed to initialize the resampling context."));
		return false;
	}

	return true;
}

struct AVFrame* FRTMPPublisher::AllocPicture(enum AVPixelFormat Format, int32 Width, int32 Height)
{
	AVFrame* Picture = av_frame_alloc();
	if (Picture == nullptr) {
		return nullptr;
	}

	Picture->format = Format;
	Picture->width = Width;
	Picture->height = Height;

	if (av_frame_get_buffer(Picture, 0) < 0) {
		UE_LOG(LogRTMPPublisher, Error, TEXT("Cloud not allocate frame data."));
	}

	return Picture;;
}

struct AVFrame* FRTMPPublisher::AllocAudioFrame(enum AVSampleFormat Format, uint64 ChannelLayout, int32 SampleRate, int32 SamplesCount)
{
	AVFrame* Frame = av_frame_alloc();

	if (Frame == nullptr) {
		UE_LOG(LogRTMPPublisher, Error, TEXT("Error allocating an audio frame."));
		return nullptr;
	}

	Frame->format = Format;
	Frame->channel_layout = ChannelLayout;
	Frame->sample_rate = SampleRate;
	Frame->nb_samples = SamplesCount;

	if (SamplesCount) {
		if (av_frame_get_buffer(Frame, 0) < 0) {
			UE_LOG(LogRTMPPublisher, Error, TEXT("Error allocating an audio buffer."));
		}
	}

	return Frame;
}

void FRTMPPublisher::CloseStream(FOutputStream& Stream)
{
	if (Stream.CodecCtx != nullptr) {
		avcodec_free_context(&Stream.CodecCtx);
		Stream.CodecCtx = nullptr;
	}
	if (Stream.Frame != nullptr) {
		av_frame_free(&Stream.Frame);
		Stream.Frame = nullptr;
	}
	if (Stream.TempFrame != nullptr) {
		av_frame_free(&Stream.TempFrame);
		Stream.TempFrame = nullptr;
	}
	if (Stream.SwsCtx != nullptr) {
		sws_freeContext(Stream.SwsCtx);
		Stream.SwsCtx = nullptr;
	}
	if (Stream.SwrCtx != nullptr) {
		swr_free(&Stream.SwrCtx);
		Stream.SwrCtx = nullptr;
	}

	Stream.SamplesCount = 0;
	Stream.NextPts = 0;
}

bool FRTMPPublisher::SendVideoFrame()
{
	AVCodecContext* CodecCtx = VideoStream.CodecCtx;

	if (av_frame_make_writable(VideoStream.Frame) < 0) {
		UE_LOG(LogFFMPEGEncoder_Video, Error, TEXT("Cloud not make frame writable."));
		return false;
	}

	if (VideoStream.SwsCtx == nullptr) {
		VideoStream.SwsCtx = sws_getContext(CodecCtx->width, CodecCtx->height, AV_PIX_FMT_BGRA,
			CodecCtx->width, CodecCtx->height, CodecCtx->pix_fmt,
			SWS_BICUBIC, nullptr, nullptr, nullptr);
		if (VideoStream.SwsCtx == nullptr) {
			UE_LOG(LogFFMPEGEncoder_Video, Error, TEXT("Cloud not initialize the conversion context."));
			return false;
		}
	}

	if (CodecCtx->pix_fmt != AV_PIX_FMT_YUV420P) {
		UE_LOG(LogFFMPEGEncoder_Video, Error, TEXT("Currently only support yuv40p data."));
		return false;
	}

	FEncodeFramePayload RawData;
	if (!VideoFrameQueue.Dequeue(RawData)) {
		return false;
	}

	VideoStream.TempFrame->data[0] = RawData.Data.GetData();
	VideoStream.TempFrame->linesize[0] = RawData.Width * 4;
	
	sws_scale(VideoStream.SwsCtx, VideoStream.TempFrame->data, VideoStream.TempFrame->linesize, 0, CodecCtx->height, VideoStream.Frame->data, VideoStream.Frame->linesize);

	VideoStream.Frame->pts = VideoStream.NextPts++;
	
	if (avcodec_send_frame(CodecCtx, VideoStream.Frame) < 0) {
		UE_LOG(LogFFMPEGEncoder_Video, Error, TEXT("Error encoding video frame."));
		return false;
	}

	AVPacket Packet = { 0 };
	av_init_packet(&Packet);
	if (avcodec_receive_packet(CodecCtx, &Packet) < 0) {
		UE_LOG(LogFFMPEGEncoder_Video, Error, TEXT("Cloud not find useful packet."));
		return false;
	}

	return SendFrameInternal(&CodecCtx->time_base, VideoStream.Stream, &Packet);
}

bool FRTMPPublisher::SendAudioFrame()
{
	int32 FrameBytes = AudioStream.TempFrame->nb_samples * AudioStream.CodecCtx->channels * 2;
	{
		FScopeLock Lock(&AudioSubmixBufferCS);
		if (AudioSubmixBuffer.Num() < FrameBytes) {
			return false;
		}

		FMemory::Memcpy(AudioStream.TempFrame->data[0], AudioSubmixBuffer.GetData(), FrameBytes);

		AudioSubmixBuffer.RemoveAt(0, FrameBytes);
	}

	AudioStream.TempFrame->pts = AudioStream.NextPts;
	AudioStream.NextPts += AudioStream.TempFrame->nb_samples;

	int32 DST_NB_Samples = av_rescale_rnd(swr_get_delay(AudioStream.SwrCtx, AudioStream.CodecCtx->sample_rate) + AudioStream.TempFrame->nb_samples,
		AudioStream.CodecCtx->sample_rate, AudioStream.CodecCtx->sample_rate, AV_ROUND_UP);

	if (av_frame_make_writable(AudioStream.Frame) < 0) {
		UE_LOG(LogFFMPEGEncoder_Audio, Error, TEXT("Cloud not make dst frame writable."));
		return false;
	}

	if (swr_convert(AudioStream.SwrCtx, AudioStream.Frame->data, DST_NB_Samples, (const uint8**)AudioStream.TempFrame->data, AudioStream.TempFrame->nb_samples) < 0) {
		UE_LOG(LogFFMPEGEncoder_Audio, Error, TEXT("Cloud not convert source frame to dst frame."));
		return false;
	}

	AudioStream.Frame->pts = av_rescale_q(AudioStream.SamplesCount, { 1, AudioStream.CodecCtx->sample_rate }, AudioStream.CodecCtx->time_base);
	AudioStream.SamplesCount += DST_NB_Samples;

	int32 Result = avcodec_send_frame(AudioStream.CodecCtx, AudioStream.Frame);
	if (Result < 0) {
		UE_LOG(LogFFMPEGEncoder_Audio, Error, TEXT("Error sending a frame to the encoder."));
		return false;
	}

	AVPacket Packet;
	FMemory::Memset(Packet, 0);
	av_init_packet(&Packet);

	while (Result >= 0)
	{
		Result = avcodec_receive_packet(AudioStream.CodecCtx, &Packet);
		if (Result == AVERROR(EAGAIN) || Result == AVERROR_EOF) {
			break;
		}
		else if (Result < 0) {
			UE_LOG(LogFFMPEGEncoder_Audio, Error, TEXT("Error encoding a frame."));
			return false;
		}

		av_packet_rescale_ts(&Packet, AudioStream.CodecCtx->time_base, AudioStream.Stream->time_base);
		Packet.stream_index = AudioStream.Stream->index;

		Result = av_interleaved_write_frame(OutputFormatCtx, &Packet);
		if (Result < 0) {
			UE_LOG(LogFFMPEGEncoder_Audio, Error, TEXT("Error while writing output packet."));
			return false;
		}
	}

	return Result == AVERROR_EOF;
}

bool FRTMPPublisher::SendFrameInternal(const struct AVRational* TimeBase, struct AVStream* Stream, struct AVPacket* Packet)
{
	av_packet_rescale_ts(Packet, *TimeBase, Stream->time_base);
	Packet->stream_index = Stream->index;

	return av_interleaved_write_frame(OutputFormatCtx, Packet) == 0;
}

void FRTMPPublisher::OnViewportRecorded(const FColor* ColorBuffer, uint32 Width, uint32 Height)
{
	FTimespan PassedTime = FDateTime::Now() - StartTime;
	FTimespan IntervalTime = FTimespan::FromMilliseconds(1000.0 / PublisherConfig.Framerate);
	FTimespan NextFrameTime = IntervalTime* CapturedVideoFrameCount;

	if (PassedTime < NextFrameTime) {
		UE_LOG(LogRTMPPublisher, Verbose, TEXT("Cloud not capture this frame when next frame time is not coming, drop it."));
		return;
	}	

	FEncodeFramePayload Payload;
	Payload.Data.Append((uint8*)ColorBuffer, Width * Height * 4);
	Payload.Width = Width;
	Payload.Height = Height;

	VideoFrameQueue.Enqueue(Payload);

	CapturedVideoFrameCount++;
}
