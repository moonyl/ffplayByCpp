#pragma once

#ifndef _VIDEO_STATE_H_
#define _VIDEO_STATE_H_

#include "Clock.h"
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avfft.h>
}
#include "PacketQueue.h"
#include "FrameQueue.h"
#include <memory>

struct SDL_cond;
struct SDL_Window;
struct SDL_Renderer;
class Decoder;
struct SwsContext;

// TODO : make this into class
struct AudioParams {
	int freq;
	int channels;
	int64_t channelLayout;
	AVSampleFormat fmt;
	int frameSize;
	int bytesPerSec;
};

class VideoState
{
public:
	VideoState(const char *filename, AVInputFormat *iformat);
	~VideoState();

public:
	enum ShowMode {
		SHOW_MODE_NONE = -1,
		SHOW_MODE_VIDEO = 0,
		SHOW_MODE_WAVES,
		SHOW_MODE_RDFT,
		SHOW_MODE_NB
	};

private:
	enum {
		DefaultWidth = 640,
		DefaultHeight = 480
	};

	enum {
		AUDIO_DIFF_AVG_NB = 20
	};
	
	enum {
		SDL_AUDIO_MIN_BUFFER_SIZE = 512
	};

	static const float AV_NOSYNC_THRESHOLD;
	
	enum {
		SAMPLE_ARRAY_SIZE = (8 * 65536)
	};
	
public:
	int masterSyncType() const;
	double getMasterClock() const;

	int openVideo();
	void displayVideo();

	void stepToNextFrame();
	void toggleStreamPause();
	void refreshLoopWaitEvent(SDL_Event &event);

private:
	int openStreamComponent(int streamIndex);
	int openAudio(int64_t wantedChannelLayout, int wantedNbChannels, int wantedSampleRate, AudioParams &audioHwParams);
	void setClockAt(Clock &c, double pts, int serial, double time);
	void syncClockToSlave(Clock &c, Clock &slave);
	void updateSampleDisplay(short *samples, int sampleSize);
	int audioDecodeFrame();
	void seekStream(int64_t pos, int64_t rel, int seekByBytes);
	void refreshVideo(double &remainingTime);
	void checkExternalClockSpeed();
	double vpDuration(Frame *vp, Frame *nextVp);
	double computeTargetDelay(double delay);
	void updateVideoPts(double pts, int64_t pos, int serial);
	int getVideoFrame(AVFrame *frame);
	int queuePicture(AVFrame *srcFrame, double pts, double duration, int64_t pos, int serial);
	void displayVideoAudio();
	void fillRectangle(int x, int y, int w, int h);
	int computeMod(int a, int b);
	int reallocTexture(SDL_Texture **texture, Uint32 newFormat, int newWidth, int newHeight, SDL_BlendMode blendMode, int initTexture);
	void displayVideoImage();

private:
	static int readThread(void *arg);
	static int decodeInterruptCb(void *ctx);
	static void sdlAudioCallback(void *opaque, Uint8 *stream, int len);
	static int audioThread(void *arg);
	static int videoThread(void *arg);

private:	// members should be zero on creating
	const char* m_filename = nullptr;
	const char* m_windowTitle = nullptr;
	AVInputFormat *m_iFormat = nullptr;
	int m_width = 0;
	int m_height = 0;
	int m_xLeft = 0;
	int m_yTop = 0;

	AVFormatContext *m_ic = nullptr;

	PacketQueue m_videoQ;
	PacketQueue m_audioQ;
	PacketQueue m_subtitleQ;

	FrameQueue m_pictureQ;
	FrameQueue m_subPictureQ;
	FrameQueue m_sampleQ;

	SDL_cond *m_continueReadThread = nullptr;

	Clock m_audClk;
	Clock m_vidClk;
	Clock m_extClk;

	int m_audioClockSerial = -1;
	int m_audioVolume = 100;	// TODO : need to clip
	
	int m_avSyncType = Clock::AV_SYNC_AUDIO_MASTER;

	SDL_Thread *m_readTid = nullptr;

	AVStream *m_audioSt = nullptr;
	AVStream *m_videoSt = nullptr;
	AVStream *m_subtitleSt = nullptr;

	int m_videoStream = -1;
	int m_audioStream = -1;
	int m_subtitleStream = -1;	

	SDL_Window *m_window = nullptr;
	SDL_Renderer *m_renderer = nullptr;

	bool m_isFullScreen = false;
	bool m_isBorderless = true;

	int m_eof = 0;

	int m_lastVideoStream = -1;
	int m_lastAudioStream = -1;
	int m_lastSubtitleStream = -1;

	double m_maxFrameDuration = 0.0;

	int64_t m_startTime = AV_NOPTS_VALUE;
	int64_t m_duration = AV_NOPTS_VALUE;

	int m_abortRequest = 0;
	bool m_realtime = false;
	bool m_showStatus = true;

	const char* m_wantedStreamSpec[AVMEDIA_TYPE_NB] = { 0 };

	bool m_videoDisable = false;
	bool m_audioDisable = false;
	bool m_subtitleDisable = false;

	ShowMode m_showMode = SHOW_MODE_VIDEO;

	int m_screenWidth = 0;
	int m_screenHeight = 0;

	int64_t m_audioCallbackTime = 0;

	int m_frameDropsEarly = 0;
	int m_frameDropsLate = 0;

	static int m_genPts;
	static int m_seekByBytes;

	static AVDictionary *m_formatOpts;
	static AVDictionary *m_codecOpts;
	static AVDictionary *m_resampleOpts;

	AudioParams m_audioTgt;
	AudioParams m_audioSrc;

	int m_audioHwBufSize = 0;
	uint8_t *m_audioBuf = nullptr;
	unsigned int m_audioBufSize = 0;
	int m_audioWriteBufSize = 0;
	int m_audioBufIndex = 0;
	int m_audioDiffAvgCount = 0;
	double m_audioDiffThreshold = 0.0;
	double m_audioDiffAvgCoef = 0.0;


	double m_audioClock = 0.0;
	int m_muted = 0;

	int m_sampleArrayIndex = 0;
	int16_t m_sampleArray[SAMPLE_ARRAY_SIZE];

	int m_paused = 0;
	int m_lastPaused = 0;
	int m_readPauseReturn = 0;

	int m_seekFlags = 0;
	int m_seekReq = 0;
	int m_queueAttachmentsReq = 0;
	int64_t m_seekPos = 0;
	int64_t m_seekRel = 0;

	int m_step = 0;
	double m_frameTimer = 0.0;

	int m_forceRefresh = 0;

	double m_lastVisTime = 0.0;
	const float AV_SYNC_THRESHOLD_MIN = 0.04;
	const float AV_SYNC_THRESHOLD_MAX = 0.1;
		
	SDL_Texture *m_subTexture = nullptr;

	double m_frameLastFilterDelay = 0.0;

	int m_lastIStart = 0;

	int m_rdftBits = 0;
	RDFTContext *m_rdft = nullptr;
	FFTSample *m_rdftData = nullptr;

	SDL_Texture *m_visTexture = nullptr;
	SDL_Texture *m_vidTexture = nullptr;

	int m_xPos = 0;

	SwsContext *m_subConvertCtx = nullptr;
	SwsContext *m_imgConvertCtx = nullptr;

	std::unique_ptr<Decoder> m_audDec;
	std::unique_ptr<Decoder> m_vidDec;
	std::unique_ptr<Decoder> m_subDec;	
};

#endif
