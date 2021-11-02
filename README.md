# Streaming
A UE4 plugin used ffmpeg library to record/publish game viewport output and in-game audio.
GPL license beacuse used ffmpeg.

# Structures
GameViewportRecorder: Record game viewport output and give you bgra corlo data. Only useful in runtime.


FRTMPPublisherConfig: Set of configs for setup RTMPPublisher, its a blueprint type struct.


RTMPPublisher: Use GameViewportRecorder, ISubmixBufferListener and ffmpeg library to record/publish game viewport output an in-game audio, not only use for rtmp protocol, can save
as file too.


RTMPPublisherComopnent: Just a simple component to test RTMPPublisher utils.


# How to use it
There is a demo in project, a UMG widget for setup config and start record/publish.

If you want to test it, just package the project then play it.

Skip StreamKey option, its not used.

Bitrate option should be write like 192kbps == 192 * 1024, beacuse ffmpeg will use the bitrate option like bit_rate / 1024;
