#include "VideoState.h"
#include <SDL.h>
#include "Clock.h"
#include <cstdint>

extern "C" {
#include <libavutil/avstring.h>
#include <libavutil/opt.h>
#include <libavfilter/buffersink.h>
#include <libavutil/time.h>
#include <libavfilter/buffersrc.h>
#include <libavcodec/avfft.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#include "Decoder.h"

#include "Renderer.h"
#include "Window.h"
#include "Thread.h"
#include "Condition.h"
#include "SwScaleContext.h"
#include "Mutex.h"
#include "SwResampleContext.h"

#define FF_QUIT_EVENT    (SDL_USEREVENT + 2)
#define REFRESH_RATE	0.01

AVDictionary *VideoState::m_formatOpts;
AVDictionary *VideoState::m_codecOpts;
AVDictionary *VideoState::m_resampleOpts;
int VideoState::m_genPts = 0;
int VideoState::m_seekByBytes = -1;

static VideoState::ShowMode s_showMode = VideoState::SHOW_MODE_NONE;
static int defaultWidth = 640;
static int defaultHeight = 480;
static int s_lowres = 0;

static const char *s_audioCodecName;
static const char *s_videoCodecName;
static const char *s_subtitleCodecName;

static int s_fast = 0;
static int s_infiniteBuffer = -1;

static int s_loop = 1;
static int s_autoexit = 0;

static int64_t s_duration = AV_NOPTS_VALUE;
static int s_cursorHidden = 0;
static int64_t s_cursorLastShown;
#define CURSOR_HIDE_DELAY	1000000

static int s_displayDisable;
static double s_rdftSpeed = 0.02;

#define EXTERNAL_CLOCK_MIN_FRAMES	2
#define EXTERNAL_CLOCK_MAX_FRAMES	10

#define EXTERNAL_CLOCK_SPEED_MIN	0.900
#define EXTERNAL_CLOCK_SPEED_MAX	1.010
#define EXTERNAL_CLOCK_SPEED_STEP	0.001

static int s_frameDrop = -1;

static int64_t s_audioCallbackTime;

static unsigned s_swsFlags = SWS_BICUBIC;

const float VideoState::AV_NOSYNC_THRESHOLD = 10.0;

VideoState::VideoState(const char * filename, AVInputFormat * iformat) :
	m_filename(av_strdup(filename)),
	m_iFormat(iformat),
	m_pictureQ(m_videoQ, FrameQueue::VIDEO_PICTURE_QUEUE_SIZE, 1),
	m_subPictureQ(m_subtitleQ, FrameQueue::SUBPICTURE_QUEUE_SIZE, 0),
	m_sampleQ(m_audioQ, FrameQueue::SAMPLE_QUEUE_SIZE, 1),
	m_condReadThread(std::make_unique<Condition>()),
	m_audClk(m_audioQ.serial()),
	m_vidClk(m_videoQ.serial()),
	m_extClk(m_subtitleQ.serial()),
	m_imgConvertCtx(std::make_unique<SwScaleContext>()),
	m_subConvertCtx(std::make_unique<SwScaleContext>()),
	m_readThread(std::make_unique<Thread>(readThread, "readThread", this)),
	m_swResampleCtx(std::make_unique<SwResampleContext>())
{
	if (!m_condReadThread) {
		av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
		// TODO : throw exception;
	}

	if (!m_readThread)	{
		av_log(NULL, AV_LOG_FATAL, "SDL_CreateThread(): %s\n", SDL_GetError());
		// TODO : throw exception;
	}
}

VideoState::~VideoState()
{
}

int VideoState::masterSyncType() const
{
	if (m_avSyncType == AV_SYNC_VIDEO_MASTER) {
		if (m_videoSt) {
			return AV_SYNC_VIDEO_MASTER;
		}
		else {
			return AV_SYNC_AUDIO_MASTER;
		}
	}
	else if (m_avSyncType == AV_SYNC_AUDIO_MASTER) {
		if (m_audioSt) {
			return AV_SYNC_AUDIO_MASTER;
		}
		else {
			return AV_SYNC_EXTERNAL_CLOCK;
		}
	}

	return AV_SYNC_EXTERNAL_CLOCK;
}

double VideoState::getMasterClock() const
{
	double val;

	switch (masterSyncType()) {
	case AV_SYNC_VIDEO_MASTER:
		val = m_vidClk.getClock();
		break;
	case AV_SYNC_AUDIO_MASTER:
		val = m_audClk.getClock();
		break;
	default:
		val = m_extClk.getClock();
		break;
	}
	return val;
}

int VideoState::openVideo()
{
	int screenWidth = defaultWidth;
	int screenHeight = defaultHeight;

	if (!m_windowTitle) {
		m_windowTitle = m_filename;
	}

	if (!m_window) {
		m_window = std::make_unique<Window>(
			Window::WindowConfig{ m_windowTitle, m_isFullScreen, m_isBorderless, screenWidth, screenHeight });

		if (m_window) {
			m_renderer = std::make_unique<Renderer>(*m_window);
		}
	}
	else {
		m_window->setSize(screenWidth, screenHeight);
	}

	if (!m_window || !m_renderer) {
		av_log(NULL, AV_LOG_FATAL, "SDL: could not set video mode - exiting\n");
		// doExit()
	}

	m_width = screenWidth;
	m_height = screenHeight;

	return 0;
}

void VideoState::displayVideo()
{
	if (!m_window) {
		openVideo();
	}

	m_renderer->setDrawColor(0, 0, 0, 255);
	m_renderer->clear();

	// TODO : execute display
	if (m_audioSt && m_showMode != SHOW_MODE_VIDEO) {
		displayVideoAudio();
	}
	else if (m_videoSt) {
		displayVideoImage();
	}

	m_renderer->present();
}

void VideoState::stepToNextFrame()
{
	if (m_paused) {
		toggleStreamPause();
	}
	m_step = 1;
}

void VideoState::toggleStreamPause()
{
	if (m_paused) {
		m_frameTimer += av_gettime_relative() / 1000000.0 - m_vidClk.lastUpdated();
		if (m_readPauseReturn != AVERROR(ENOSYS)) {
			m_vidClk.setPaused(0);
		}
		m_vidClk.setClock(m_vidClk.getClock(), m_vidClk.serial());
	}
	m_extClk.setClock(m_extClk.getClock(), m_extClk.serial());
	m_paused = !m_paused;
	m_audClk.setPaused(m_paused);
	m_vidClk.setPaused(m_paused);
	m_extClk.setPaused(m_paused);
}

void VideoState::refreshLoopWaitEvent(SDL_Event & event)
{
	double remainingTime = 0.0;
	SDL_PumpEvents();
	while (!SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
		if (!s_cursorHidden && av_gettime_relative() - s_cursorLastShown > CURSOR_HIDE_DELAY) {
			SDL_ShowCursor(0);
			s_cursorHidden = 1;
		}
		if (remainingTime > 0.0) {
			av_usleep((int64_t)(remainingTime * 1000000.0));
		}
		remainingTime = REFRESH_RATE;
		if (m_showMode != SHOW_MODE_NONE && (!m_paused || m_forceRefresh)) {
			refreshVideo(remainingTime);
		}
		SDL_PumpEvents();
	}
}

static int checkStreamSpecifier(AVFormatContext *s, AVStream *st, const char *spec)
{
	int ret = avformat_match_stream_specifier(s, st, spec);
	if (ret < 0) {
		av_log(s, AV_LOG_ERROR, "Invalid stream specifier: %s.\n", spec);
	}
	return ret;
}

static AVDictionary *filterCodecOpts(AVDictionary *opts, AVCodecID codecId, AVFormatContext *s, AVStream *st, AVCodec *codec)
{
	AVDictionary *ret = nullptr;
	AVDictionaryEntry *t = nullptr;

	int flags = s->oformat ? AV_OPT_FLAG_ENCODING_PARAM : AV_OPT_FLAG_DECODING_PARAM;
	char prefix = 0;
	const AVClass *cc = avcodec_get_class();

	if (!codec) {
		codec = s->oformat ? avcodec_find_encoder(codecId) : avcodec_find_decoder(codecId);
	}

	switch (st->codecpar->codec_type) {
	case AVMEDIA_TYPE_VIDEO:
		prefix = 'v';
		flags |= AV_OPT_FLAG_VIDEO_PARAM;
		break;
	case AVMEDIA_TYPE_AUDIO:
		prefix = 'a';
		flags |= AV_OPT_FLAG_AUDIO_PARAM;
		break;
	case AVMEDIA_TYPE_SUBTITLE:
		prefix = 's';
		flags |= AV_OPT_FLAG_SUBTITLE_PARAM;
		break;
	}

	while (t = av_dict_get(opts, "", t, AV_DICT_IGNORE_SUFFIX)) {
		char *p = strchr(t->key, ':');

		if (p) {
			switch (checkStreamSpecifier(s, st, p + 1)) {
			case 1:
				*p = 0;
				break;
			case 0:
				continue;
			default:
				exit(1);
			}
		}

		if (av_opt_find(&cc, t->key, nullptr, flags, AV_OPT_SEARCH_FAKE_OBJ) ||
			!codec ||
			(codec->priv_class &&
				av_opt_find(&codec->priv_class, t->key, nullptr, flags, AV_OPT_SEARCH_FAKE_OBJ))) {
			av_dict_set(&ret, t->key, t->value, 0);
		}
		else if (t->key[0] == prefix &&
			av_opt_find(&cc, t->key + 1, nullptr, flags, AV_OPT_SEARCH_FAKE_OBJ)) {
			av_dict_set(&ret, t->key + 1, t->value, 0);
		}

		if (p) {
			*p = ':';
		}
	}
	return ret;
}

int VideoState::openStreamComponent(int streamIndex)
{
	AVFormatContext *ic = m_ic;
	AVCodecContext *avctx;
	AVCodec *codec;
	const char *forcedCodecName = nullptr;
	AVDictionary *opts = nullptr;
	AVDictionaryEntry *t = nullptr;
	int sampleRate, nbChannels;
	int64_t channelLayout;
	int ret = 0;
	int streamLowres = s_lowres;
	
	if (streamIndex < 0 || streamIndex >= ic->nb_streams) {
		return -1;
	}

	avctx = avcodec_alloc_context3(nullptr);
	if (!avctx) {
		return AVERROR(ENOMEM);
	}

	ret = avcodec_parameters_to_context(avctx, ic->streams[streamIndex]->codecpar);
	if (ret < 0) {
		// TODO : handle error
	}
	av_codec_set_pkt_timebase(avctx, ic->streams[streamIndex]->time_base);

	codec = avcodec_find_decoder(avctx->codec_id);

	switch (avctx->codec_type) {
	case AVMEDIA_TYPE_AUDIO:
		m_lastAudioStream = streamIndex;
		forcedCodecName = s_audioCodecName;
		break;
	case AVMEDIA_TYPE_SUBTITLE:
		m_lastSubtitleStream = streamIndex;
		forcedCodecName = s_subtitleCodecName;
		break;
	case AVMEDIA_TYPE_VIDEO:
		m_lastVideoStream = streamIndex;
		forcedCodecName = s_videoCodecName;
		break;
	}

	if (forcedCodecName) {
		codec = avcodec_find_decoder_by_name(forcedCodecName);
	}

	if (!codec) {
		if (forcedCodecName) {
			av_log(nullptr, AV_LOG_WARNING, "No codec could be found with name '%s'\n", forcedCodecName);
		}
		else {
			av_log(nullptr, AV_LOG_WARNING, "No codec could be found with id %d\n", avctx->codec_id);
		}
		ret = AVERROR(EINVAL);
		// TODO : handle error
	}

	avctx->codec_id = codec->id;
	/**
	 * low resolution decoding, 1-> 1/2 size, 2->1/4 size
	 */
	if (streamLowres > av_codec_get_max_lowres(codec)) {
		av_log(avctx, AV_LOG_WARNING, "The maximum value for lowres supported by the decoder is %d\n",
			av_codec_get_max_lowres(codec));
		streamLowres = av_codec_get_max_lowres(codec);
	}
	av_codec_set_lowres(avctx, streamLowres);

#if FF_API_EMU_EDGE
	if (streamLowres) {
		avctx->flags |= CODEC_FLAG_EMU_EDGE;
	}
#endif
	if (s_fast) {
		avctx->flags2 |= AV_CODEC_FLAG2_FAST;
#if FF_API_EMU_EDGE
		if (codec->capabilities & AV_CODEC_CAP_DR1) {
			avctx->flags |= CODEC_FLAG_EMU_EDGE;
		}
#endif
	}

	opts = filterCodecOpts(m_codecOpts, avctx->codec_id, ic, ic->streams[streamIndex], codec);
	if (!av_dict_get(opts, "threads", nullptr, 0)) {
		av_dict_set(&opts, "threads", "auto", 0);
	}
	if (streamLowres) {
		av_dict_set_int(&opts, "lowres", streamLowres, 0);
	}
	if (avctx->codec_type == AVMEDIA_TYPE_VIDEO || avctx->codec_type == AVMEDIA_TYPE_AUDIO) {
		av_dict_set(&opts, "refcounted_frames", "1", 0);
	}
	if ((ret = avcodec_open2(avctx, codec, &opts)) < 0) {
		// TODO : handle error
	}
	if ((t = av_dict_get(opts, "", nullptr, AV_DICT_IGNORE_SUFFIX))) {
		av_log(nullptr, AV_LOG_ERROR, "Option %s not found.\n", t->key);
		ret = AVERROR_OPTION_NOT_FOUND;
		// TODO : handle error
	}

	m_eof = 0;
	ic->streams[streamIndex]->discard = AVDISCARD_DEFAULT;

	switch (avctx->codec_type) {
	case AVMEDIA_TYPE_AUDIO:
#if CONFIG_AVFILTER
	{
		AVFilterContext *sink;
		m_audioFilterSrc.freq = avctx->sample_rate;
		m_audioFilterSrc.channels = avctx->channels;
		m_audioFilterSrc.channel_layout = getValidChannelLayout(avctx->channel_layout, avctx->channels);
		m_audioFilterSrc.fmt = avctx->sample_fmt;
		if ((ret = configureAudioFilters(aFilters, 0) < 0) {
			// TODO : handle error
		}
		sink = m_outAudioFilter;
		sampleRate = av_buffersink_get_sample_rate(sink);
		nbChannels = av_buffersink_get_channels(sink);
		channelLayout = av_buffersink_get_channel_layout(sink);
	}
#else
		sampleRate = avctx->sample_rate;
		nbChannels = avctx->channels;
		channelLayout = avctx->channel_layout;
#endif
		if ((ret = openAudio(channelLayout, nbChannels, sampleRate, m_audioTgt)) < 0) {
			// TODO : handle error
		}
		m_audioHwBufSize = ret;
		m_audioSrc = m_audioTgt;
		m_audioBufSize = 0;
		m_audioBufIndex = 0;

		m_audioDiffAvgCoef = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
		m_audioDiffAvgCount = 0;
		m_audioDiffThreshold = (double)(m_audioHwBufSize / m_audioTgt.bytesPerSec);

		m_audioStream = streamIndex;
		m_audioSt = ic->streams[streamIndex];

		m_audDec.reset(new Decoder(avctx, m_audioQ, *m_condReadThread.get()));
		if ((m_ic->iformat->flags & (AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK))
			&& !m_ic->iformat->read_seek) {
			m_audDec->setStartPts(m_audioSt->start_time);
			m_audDec->setStartPtsTb(m_audioSt->time_base);
		}
		if ((ret = m_audDec->start(audioThread, this)) < 0) {
			// TODO : throw exception
		}
		SDL_PauseAudio(0);
		break;
	case AVMEDIA_TYPE_VIDEO:
		m_videoStream = streamIndex;
		m_videoSt = ic->streams[streamIndex];

		m_vidDec.reset(new Decoder(avctx, m_videoQ, *m_condReadThread.get()));
		if ((ret = m_vidDec->start(videoThread, this)) < 0) {
			// TODO : throw exception
		}
		break;
	case AVMEDIA_TYPE_SUBTITLE:
		m_subtitleStream = streamIndex;
		m_subtitleSt = ic->streams[streamIndex];
		m_subDec.reset(new Decoder(avctx, m_subtitleQ, *m_condReadThread.get()));
		if ((ret = m_subDec->start(subTitleThread, this)) < 0) {
			// TODO : throw exception
		}
		break;
	default:
		break;
	}
	goto out;

fail:
	avcodec_free_context(&avctx);

out:
	av_dict_free(&opts);

	return ret;
}


int VideoState::openAudio(int64_t wantedChannelLayout, int wantedNbChannels, int wantedSampleRate, AudioParams & audioHwParams)
{
	SDL_AudioSpec wantedSpec, spec;
	const char *env;
	static const int nextNbChannels[] = { 0, 0, 1, 6, 2, 6, 4, 6 };
	static const int nextSampleRates[] = { 0, 44100, 48000, 96000, 192000 };
	int nextSampleRateIdx = FF_ARRAY_ELEMS(nextSampleRates) - 1;

	env = SDL_getenv("SDL_AUDIO_CHANNELS");
	if (env) {
		wantedNbChannels = atoi(env);
		wantedChannelLayout = av_get_default_channel_layout(wantedNbChannels);
	}
	if (!wantedChannelLayout || wantedNbChannels != av_get_channel_layout_nb_channels(wantedChannelLayout)) {
		wantedChannelLayout = av_get_default_channel_layout(wantedNbChannels);
		wantedChannelLayout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
	}

	wantedNbChannels = av_get_channel_layout_nb_channels(wantedChannelLayout);
	wantedSpec.channels = wantedNbChannels;
	wantedSpec.freq = wantedSampleRate;
	if (wantedSpec.freq <= 0 || wantedSpec.channels <= 0) {
		av_log(nullptr, AV_LOG_ERROR, "Invaild sample rate or channel count!\n");
		return -1;
	}

	while (nextSampleRateIdx && nextSampleRates[nextSampleRateIdx] >= wantedSpec.freq) {
		nextSampleRateIdx--;
	}
	wantedSpec.format = AUDIO_S16SYS;
	wantedSpec.silence = 0;
	const int SDL_AUDIO_MAX_CALLBACKS_PER_SEC = 30;
	wantedSpec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wantedSpec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
	wantedSpec.callback = sdlAudioCallback;
	wantedSpec.userdata = this;
	while (SDL_OpenAudio(&wantedSpec, &spec) < 0) {
		av_log(nullptr, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n",
			wantedSpec.channels, wantedSpec.freq, SDL_GetError());
		wantedSpec.channels = nextNbChannels[FFMIN(7, wantedSpec.channels)];
		if (!wantedSpec.channels) {
			wantedSpec.freq = nextSampleRates[nextSampleRateIdx--];
			wantedSpec.channels = wantedNbChannels;
			if (!wantedSpec.freq) {
				av_log(nullptr, AV_LOG_ERROR, "No more combinations to try, audio open failed\n");
				return -1;
			}
		}
		wantedChannelLayout = av_get_default_channel_layout(wantedSpec.channels);
	}
	if (spec.format != AUDIO_S16SYS) {
		av_log(nullptr, AV_LOG_ERROR, "SDL advised audio format %d is not supported!\n", spec.format);
		return -1;
	}
	if (spec.channels != wantedSpec.channels) {
		wantedChannelLayout = av_get_default_channel_layout(spec.channels);
		if (!wantedChannelLayout) {
			av_log(nullptr, AV_LOG_ERROR, "SDL advised channel count %d is not supported!\n", spec.channels);
			return -1;
		}
	}

	audioHwParams.fmt = AV_SAMPLE_FMT_S16;
	audioHwParams.freq = spec.freq;
	audioHwParams.channelLayout = wantedChannelLayout;
	audioHwParams.channels = spec.channels;
	audioHwParams.frameSize = av_samples_get_buffer_size(nullptr, audioHwParams.channels, 1, audioHwParams.fmt, 1);
	audioHwParams.bytesPerSec = av_samples_get_buffer_size(nullptr, audioHwParams.channels, audioHwParams.freq, audioHwParams.fmt, 1);
	if (audioHwParams.bytesPerSec <= 0 || audioHwParams.frameSize <= 0) {
		av_log(nullptr, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
		return -1;
	}

	return spec.size;
}

void VideoState::setClockAt(Clock & c, double pts, int serial, double time)
{
	c.setClockAt(pts, serial, time);
}

void VideoState::syncClockToSlave(Clock & c, Clock & slave)
{
	double clock = c.getClock();
	double slaveClock = slave.getClock();
	if (!isnan(slaveClock) && (isnan(clock) || fabs(clock - slaveClock) > AV_NOSYNC_THRESHOLD)) {
		c.setClock(slaveClock, slave.serial());
	}
}

void VideoState::updateSampleDisplay(short * samples, int sampleSize)
{
	int size, len;
	size = sampleSize / sizeof(short);
	while (size > 0) {
		len = SAMPLE_ARRAY_SIZE - m_sampleArrayIndex;
		if (len > size) {
			len = size;
		}
		memcpy(m_sampleArray + m_sampleArrayIndex, samples, len * sizeof(short));
		samples += len;
		m_sampleArrayIndex += len;
		if (m_sampleArrayIndex > SAMPLE_ARRAY_SIZE) {
			m_sampleArrayIndex = 0;
		}
		size -= len;
	}
}

int VideoState::synchronizeAudio(int nbSamples)
{
	int wantedNbSamples = nbSamples;
	const int SAMPLE_CORRECTION_PERCENT_MAX = 10;

	if (masterSyncType() != AV_SYNC_AUDIO_MASTER) {
		double diff, avgDiff;
		int minNbSamples, maxNbSamples;

		diff = m_audClk.getClock() - getMasterClock();

		if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
			m_audioDiffCum = diff + m_audioDiffAvgCoef * m_audioDiffCum;
			if (m_audioDiffAvgCount < AUDIO_DIFF_AVG_NB) {
				m_audioDiffAvgCount++;
			}
			else {
				avgDiff = m_audioDiffCum * (1.0 - m_audioDiffAvgCoef);

				if (fabs(avgDiff) >= m_audioDiffThreshold) {
					wantedNbSamples = nbSamples + (int)(diff * m_audioSrc.freq);
					minNbSamples = ((nbSamples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
					maxNbSamples = ((nbSamples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
					wantedNbSamples = av_clip(wantedNbSamples, minNbSamples, maxNbSamples);
				}
				av_log(nullptr, AV_LOG_TRACE, "diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n",
					diff, avgDiff, wantedNbSamples = nbSamples, m_audioClock, m_audioDiffThreshold);
			}
		}
		else {
			m_audioDiffAvgCount = 0;
			m_audioDiffCum = 0;
		}
	}
	return wantedNbSamples;
}

int VideoState::audioDecodeFrame()
{
	int dataSize, resampledDataSize;
	int64_t decChannelLayout;
	av_unused double audioClock0;
	int wantedNbSamples;
	Frame *af;

	if (m_paused) {
		return -1;
	}

	do {
#if defined(_WIN32)
		while (m_sampleQ.remaining() == 0) {
			if ((av_gettime_relative() - m_audioCallbackTime) > 1000000LL * m_audioHwBufSize / m_audioTgt.bytesPerSec / 2)	{
				return -1;
			}
			av_usleep(1000);
		}
#endif
		if (!(af = m_sampleQ.peekReadable())) {
			return -1;
		}
		m_sampleQ.next();
	} while (af->serial() != m_audioQ.serial());

	dataSize = av_samples_get_buffer_size(nullptr, af->channels(), af->nbSamples(), af->frameFormat(), 1);
	decChannelLayout = (af->channelLayout() && af->channels() == av_get_channel_layout_nb_channels(af->channelLayout()) ?
		af->channelLayout() : av_get_default_channel_layout(af->channels()));
	wantedNbSamples = synchronizeAudio(af->nbSamples());

	if (af->frameFormat() != m_audioSrc.fmt ||
		decChannelLayout != m_audioSrc.channelLayout ||
		af->frame()->sample_rate != m_audioSrc.freq ||
		(wantedNbSamples != af->nbSamples() && !m_swResampleCtx->isApplied())) {
		if (m_swResampleCtx->applyOptionedContext(m_audioTgt.channelLayout, m_audioTgt.fmt, m_audioTgt.freq,
			decChannelLayout, af->frameFormat(), af->frame()->sample_rate) < 0) {
			return -1;
		}
		m_audioSrc.channelLayout = decChannelLayout;
		m_audioSrc.channels = af->frame()->channels;
		m_audioSrc.freq = af->frame()->sample_rate;
		m_audioSrc.fmt = af->frameFormat();
	}

	if (m_swResampleCtx->isApplied())	{

		const uint8_t **in = (const uint8_t **)af->frame()->extended_data;
		uint8_t **out = &m_audioBuf1;
		int outCount = (int64_t)wantedNbSamples * m_audioTgt.freq / af->frame()->sample_rate + 256;
		int outSize = av_samples_get_buffer_size(nullptr, m_audioTgt.channels, outCount, m_audioTgt.fmt, 0);
		int len2;
		if (outSize < 0) {
			av_log(nullptr, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
			return -1;
		}
		if (wantedNbSamples != af->frame()->nb_samples) {
			if (m_swResampleCtx->setCompensation((wantedNbSamples - af->frame()->nb_samples) * m_audioTgt.freq / af->frame()->sample_rate,
				wantedNbSamples * m_audioTgt.freq / af->frame()->sample_rate) < 0) {
				av_log(nullptr, AV_LOG_ERROR, "swr_set_compenstation() failed\n");
				return -1;
			}
		}
		av_fast_malloc(&m_audioBuf1, &m_audioBuf1Size, outSize);
		if (!m_audioBuf1) {
			return AVERROR(ENOMEM);
		}
		len2 = m_swResampleCtx->convert(out, outCount, in, af->frame()->nb_samples);
		if (len2 < 0) {
			av_log(nullptr, AV_LOG_ERROR, "swr_convert() failed\n");
			return -1;
		}
		if (len2 == outCount) {
			av_log(nullptr, AV_LOG_WARNING, "audio buffer is probably too small\n");
			if (m_swResampleCtx->init() < 0) {
				m_swResampleCtx.reset();
			}
		}
		m_audioBuf = m_audioBuf1;
		resampledDataSize = len2 * m_audioTgt.channels * av_get_bytes_per_sample(m_audioTgt.fmt);
	}
	else {
		m_audioBuf = af->frame()->data[0];
		resampledDataSize = dataSize;
	}

	audioClock0 = m_audioClock;
	if (!isnan(af->pts())) {
		m_audioClock = af->pts() + (double)af->frame()->nb_samples / af->frame()->sample_rate;
	}
	else {
		m_audioClock = NAN;
	}
	m_audioClockSerial = af->serial();
#ifdef DEBUG
	{
		static double lastClock;
		printf("audio: delay=%0.3f clock=%0.3f clock0=%0.3f\n",
			m_audioClock - lastClock, m_audioClock, audioClock0);
		lastClock = m_audioClock;
	}
#endif
	return resampledDataSize;
}

void VideoState::seekStream(int64_t pos, int64_t rel, int seekByBytes)
{
	if (!m_seekReq) {
		m_seekPos = pos;
		m_seekRel = rel;
		m_seekFlags &= ~AVSEEK_FLAG_BYTE;
		if (seekByBytes) {
			m_seekFlags |= AVSEEK_FLAG_BYTE;
		}
		m_seekReq = 1;
		m_condReadThread->signal();
	}
}

void VideoState::refreshVideo(double & remainingTime)
{
	double time;
	Frame *sp, *sp2;

	if (m_paused && masterSyncType() == AV_SYNC_EXTERNAL_CLOCK && m_realtime) {
		checkExternalClockSpeed();
	}

	if (!s_displayDisable && m_showMode != SHOW_MODE_VIDEO && m_audioSt) {
		time = av_gettime_relative() / 1000000.0;
		if (m_forceRefresh || m_lastVisTime + s_rdftSpeed < time) {
			displayVideo();
			m_lastVisTime = time;
		}
		remainingTime = FFMIN(remainingTime, m_lastVisTime + s_rdftSpeed - time);
	}

	if (m_videoSt) {
retry:
		//int level = av_log_get_level();
		//av_log_set_level(AV_LOG_DEBUG);
		//av_log(nullptr, AV_LOG_DEBUG, "remaining = %d\n", m_pictureQ.remaining());
		//av_log_set_level(level);
		if (m_pictureQ.remaining() == 0) {			
			// nothing to do
		}
		else {
			double lastDuration, duration, delay;
			Frame *vp, *lastVp;

			lastVp = m_pictureQ.peekLast();
			vp = m_pictureQ.peek();

			if (vp->serial() != m_videoQ.serial()) {
				m_pictureQ.next();
				goto retry;
			}

			if (lastVp->serial() != vp->serial()) {
				m_frameTimer = av_gettime_relative() / 1000000.0;
			}

			if (m_paused) {
				goto display;
			}

			lastDuration = vpDuration(lastVp, vp);
			delay = computeTargetDelay(lastDuration);

			time = av_gettime_relative() / 1000000.0;
			if (time < m_frameTimer + delay) {
				remainingTime = FFMIN(m_frameTimer + delay - time, remainingTime);
				goto display;
			}

			m_frameTimer += delay;

			if (delay > 0 && time - m_frameTimer > AV_SYNC_THRESHOLD_MAX) {
				m_frameTimer = time;
			}

			m_pictureQ.lock();
			if (!isnan(vp->pts())) {
				updateVideoPts(vp->pts(), vp->pos(), vp->serial());
			}

			m_pictureQ.unlock();

			if (m_pictureQ.remaining() > 1) {
				Frame *nextVp = m_pictureQ.peekNext();
				duration = vpDuration(vp, nextVp);
				if (!m_step && (s_frameDrop > 0 || (s_frameDrop && masterSyncType() != AV_SYNC_VIDEO_MASTER)) &&
					time > m_frameTimer + duration) {
					m_frameDropsLate++;
					m_pictureQ.next();
					goto retry;
				}
			}

			if (m_subtitleSt) {
				while (m_subPictureQ.remaining() > 0) {
					sp = m_subPictureQ.peek();

					if (m_subPictureQ.remaining() > 1) {
						sp2 = m_subPictureQ.peekNext();
					}
					else {
						sp2 = nullptr;
					}

					if (sp->serial() != m_subtitleQ.serial() ||
						(m_vidClk.pts() > (sp->pts() + ((float)sp->sub()->end_display_time / 1000))) ||
						(sp2 && m_vidClk.pts() > (sp2->pts() + ((float)sp2->sub()->start_display_time / 1000)))) {
						if (sp->uploaded()) {
							int i;
							for (i = 0; i < sp->sub()->num_rects; i++) {
								AVSubtitleRect *subRect = sp->sub()->rects[i];
								uint8_t *pixels;
								int pitch, j;

								if (!SDL_LockTexture(m_subTexture, (SDL_Rect *)subRect, (void **)&pixels, &pitch)) {
									for (j = 0; j < subRect->h; j++, pixels += pitch) {
										memset(pixels, 0, subRect->w << 2);
									}
									SDL_UnlockTexture(m_subTexture);
								}
							}
						}
						m_subPictureQ.next();
					}
					else {
						break;
					}
				}
			}
			m_pictureQ.next();
			m_forceRefresh = 1;

			if (m_step && !m_paused) {
				toggleStreamPause();
			}
		}
display:
		if (!s_displayDisable && m_forceRefresh && m_showMode == SHOW_MODE_VIDEO && m_pictureQ.rIndexShown()) {
			displayVideo();
		}
	}
	m_forceRefresh = 0;
	if (m_showStatus) {
		static int64_t lastTime;
		int64_t curTime;
		int aqSize, vqSize, sqSize;
		double avDiff;

		curTime = av_gettime_relative();
		if (!lastTime || (curTime - lastTime) >= 30000) {
			aqSize = 0;
			vqSize = 0;
			sqSize = 0;
			if (m_audioSt) {
				aqSize = m_audioQ.size();
			}
			if (m_videoSt) {
				vqSize = m_videoQ.size();
			}
			if (m_subtitleSt) {
				sqSize = m_subtitleQ.size();
			}
			avDiff = 0;
			if (m_audioSt && m_videoSt) {
				avDiff = m_audClk.getClock() - m_vidClk.getClock();
			}
			else if (m_videoSt) {
				avDiff = getMasterClock() - m_vidClk.getClock();
			}
			else if (m_audioSt) {
				avDiff = getMasterClock() - m_audClk.getClock();
			}
			
			av_log(nullptr, AV_LOG_INFO,
				"%7.2f %s:%7.3f fd=%4d aq=%5dKB vq=%5dB sq=%5dB f=%" PRId64 "/%" PRId64 "	\r",
				getMasterClock(),
				(m_audioSt && m_videoSt) ? "A-V" : (m_videoSt ? "M-V" : (m_audioSt ? "M-A" : "   ")),
				avDiff,
				m_frameDropsEarly + m_frameDropsLate,
				aqSize / 1024,
				vqSize / 1024,
				sqSize,
				m_videoSt ? m_vidDec->avctx()->pts_correction_num_faulty_dts : 0,
				m_videoSt ? m_vidDec->avctx()->pts_correction_num_faulty_dts : 0);
			fflush(stdout);
			lastTime = curTime;
		}
	}
}

void VideoState::checkExternalClockSpeed()
{
	if (m_videoStream >= 0 && m_videoQ.nbPackets() <= EXTERNAL_CLOCK_MIN_FRAMES ||
		m_audioStream >= 0 && m_audioQ.nbPackets() <= EXTERNAL_CLOCK_MIN_FRAMES) {
		m_extClk.setClockSpeed(FFMAX(EXTERNAL_CLOCK_SPEED_MIN, m_extClk.speed() - EXTERNAL_CLOCK_SPEED_STEP));
	}
	else if ((m_videoStream < 0 || m_videoQ.nbPackets() > EXTERNAL_CLOCK_MAX_FRAMES) &&
		(m_audioStream < 0 || m_audioQ.nbPackets() > EXTERNAL_CLOCK_MAX_FRAMES))	{
		m_extClk.setClockSpeed(FFMIN(EXTERNAL_CLOCK_SPEED_MAX, m_extClk.speed() + EXTERNAL_CLOCK_SPEED_STEP));
	}
	else {
		double speed = m_extClk.speed();
		if (speed != 1.0) {
			m_extClk.setClockSpeed(speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));
		}
	}
}

double VideoState::vpDuration(Frame * vp, Frame * nextVp)
{
	if (vp->serial() == nextVp->serial()) {
		double duration = nextVp->pts() - vp->pts();
		if (isnan(duration) || duration <= 0 || duration > m_maxFrameDuration) {
			return vp->duration();
		}
		else {
			return duration;
		}
	}
	else {
		return 0.0;
	}
}

double VideoState::computeTargetDelay(double delay)
{
	double syncThreshold, diff = 0;

	const float AV_SYNC_FRAMEDUP_THRESHOLD = 0.1;
	if (masterSyncType() != AV_SYNC_AUDIO_MASTER) {
		diff = m_vidClk.getClock() - getMasterClock();

		syncThreshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
		if (!isnan(diff) && fabs(diff) < m_maxFrameDuration) {
			if (diff <= syncThreshold) {
				delay = FFMAX(0, delay + diff);
			}
			else if (diff >= syncThreshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD) {
				delay = delay + diff;
			}
			else if (diff >= syncThreshold) {
				delay = 2 * delay;
			}
		}
	}

	av_log(nullptr, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n", delay, -diff);
	return delay;
}

void VideoState::updateVideoPts(double pts, int64_t pos, int serial)
{
	m_vidClk.setClock(pts, serial);
	m_extClk.syncClock(m_vidClk);
}

int VideoState::getVideoFrame(AVFrame * frame)
{
	int gotPicture;

	if ((gotPicture = m_vidDec->decodeFrame(frame, nullptr)) < 0) {
		return -1;
	}

	if (gotPicture) {
		double dpts = NAN;

		if (frame->pts != AV_NOPTS_VALUE) {
			dpts = av_q2d(m_videoSt->time_base) * frame->pts;
		}
		frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(m_ic, m_videoSt, frame);

		if (s_frameDrop > 0 || (s_frameDrop && masterSyncType() != AV_SYNC_VIDEO_MASTER)) {
			if (frame->pts != AV_NOPTS_VALUE) {
				double diff = dpts - getMasterClock();
				if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
					diff - m_frameLastFilterDelay < 0 &&
					m_vidDec->pktSerial() == m_vidClk.serial() &&
					m_videoQ.nbPackets()) {
					m_frameDropsEarly++;
					av_frame_unref(frame);
					gotPicture = 0;
				}
			}
		}
	}
	return gotPicture;
}

static void calculateDisplayRect(SDL_Rect *rect, int scrXleft, int scrYtop, int scrWidth, int scrHeight,
	int picWidth, int picHeight, AVRational picSar)
{
	float aspectRatio;
	int width, height, x, y;

	if (picSar.num == 0) {
		aspectRatio = 0;
	}
	else {
		aspectRatio = av_q2d(picSar);
	}

	if (aspectRatio <= 0.0) {
		aspectRatio = 1.0;
	}
	aspectRatio *= (float)picWidth / (float)picHeight;

	height = scrHeight;
	width = lrint(height * aspectRatio) & ~1;

	if (width > scrWidth) {
		width = scrWidth;
		height = lrint(width / aspectRatio) & ~1;
	}

	x = (scrWidth - width) / 2;
	y = (scrHeight - height) / 2;
	rect->x = scrXleft + x;
	rect->y = scrYtop + y;
	rect->w = FFMAX(width, 1);
	rect->h = FFMAX(height, 1);
}

static void setDefaultWindowSize(int width, int height, AVRational sar)
{
	SDL_Rect rect;
	calculateDisplayRect(&rect, 0, 0, INT_MAX, height, width, height, sar);
	defaultWidth = rect.w;
	defaultHeight = rect.h;
}

int VideoState::queuePicture(AVFrame * srcFrame, double pts, double duration, int64_t pos, int serial)
{
	Frame *vp;
#if defined(DEBUG_SYNC)
	printf("frame_type=%c pts=%0.3f\n",
		av_get_picture_type_char(srcFrame->pict_type), pts);
#endif

	if (!(vp = m_pictureQ.peekWritable())) {
		return -1;
	}

	vp->setSAR(srcFrame->sample_aspect_ratio);
	vp->setUploaded(0);
	vp->setPictureInfo(srcFrame->width, srcFrame->height, srcFrame->format);

	vp->setPosInfo(pts, duration, serial, pos);

	setDefaultWindowSize(vp->width(), vp->height(), vp->sar());

	av_frame_move_ref(vp->frame(), srcFrame);
	m_pictureQ.push();

	return 0;
}

void VideoState::fillRectangle(int x, int y, int w, int h)
{
	SDL_Rect rect;
	rect.x = x;
	rect.y = y;
	rect.w = w;
	rect.h = h;
	if (w && h) {
		m_renderer->fillRectangle(rect);
	}
}

int VideoState::computeMod(int a, int b)
{
	return a < 0 ? a%b + b : a%b;
}

int VideoState::reallocTexture(SDL_Texture ** texture, Uint32 newFormat, int newWidth, int newHeight, SDL_BlendMode blendMode, int initTexture)
{
	Uint32 format;
	int access, w, h;
	if (SDL_QueryTexture(*texture, &format, &access, &w, &h) < 0 ||
		newWidth != h || newFormat != format) {
		void *pixels;
		int pitch;
		SDL_DestroyTexture(*texture);
		if (!(*texture = SDL_CreateTexture(m_renderer->renderer(), newFormat, SDL_TEXTUREACCESS_STREAMING, newWidth, newHeight))) {
			return -1;
		}
		if (SDL_SetTextureBlendMode(*texture, blendMode) < 0) {
			return -1;
		}
		if (initTexture) {
			if (SDL_LockTexture(*texture, nullptr, &pixels, &pitch) < 0) {
				return -1;
			}
			memset(pixels, 0, pitch * newHeight);
			SDL_UnlockTexture(*texture);
		}
	}
	return 0;
}

int VideoState::uploadTexture(SDL_Texture *tex, AVFrame *frame) 
{
	int ret = 0;
	
	switch (frame->format) {
	case AV_PIX_FMT_YUV420P:
		if (frame->linesize[0] < 0 || frame->linesize[1] < 0 || frame->linesize[2] < 0) {
			av_log(nullptr, AV_LOG_ERROR, "Negative linesize is not supported for YUV.\n");
			return -1;
		}
		ret = SDL_UpdateYUVTexture(tex, nullptr, frame->data[0], frame->linesize[0],
			frame->data[1], frame->linesize[1], frame->data[2], frame->linesize[2]);
		break;
	case AV_PIX_FMT_RGBA:
		if (frame->linesize[0] < 0) {
			ret = SDL_UpdateTexture(tex, nullptr, frame->data[0] + frame->linesize[0] * (frame->height - 1), -frame->linesize[0]);
		}
		else {
			ret = SDL_UpdateTexture(tex, nullptr, frame->data[0], frame->linesize[0]);
		}
		break;
	default:
		bool retVal = m_imgConvertCtx->applyCachedContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format), frame->width, frame->height,
			AV_PIX_FMT_BGRA, s_swsFlags);
		if (retVal) {
			uint8_t *pixels[4];
			int pitch[4];
			if (!SDL_LockTexture(tex, nullptr, (void**)pixels, pitch)) {
				m_imgConvertCtx->scale((const uint8_t * const *)frame->data, frame->linesize, 0, frame->height, pixels, pitch);
				SDL_UnlockTexture(tex);
			}
		}
		else {
			av_log(nullptr, AV_LOG_FATAL, "cannot initialize the conversion context\n");
			ret = -1;
		}
		break;
	}
	return ret;
}

void VideoState::displayVideoImage()
{
	Frame *vp;
	Frame *sp = nullptr;
	SDL_Rect rect;

	vp = m_pictureQ.peekLast();
	if (m_subtitleSt) {
		if (m_subPictureQ.remaining() > 0) {
			sp = m_subPictureQ.peek();

			if (vp->pts() >= sp->pts() + ((float)sp->sub()->start_display_time / 1000)) {
				if (!sp->uploaded()) {
					uint8_t *pixels[4];
					int pitch[4];
					int i;
					if (!m_width || !m_height) {
						sp->setWidth(vp->width());
						sp->setHeight(vp->height());
					}

					if (reallocTexture(&m_subTexture, SDL_PIXELFORMAT_ABGR8888, m_width, m_height, SDL_BLENDMODE_BLEND, 1) < 0) {
						return;
					}

					for (i = 0; i < sp->sub()->num_rects; i++) {
						AVSubtitleRect *subRect = sp->sub()->rects[i];

						subRect->x = av_clip(subRect->x, 0, sp->width());
						subRect->y = av_clip(subRect->y, 0, sp->height());
						subRect->w = av_clip(subRect->w, 0, sp->width() - subRect->x);
						subRect->h = av_clip(subRect->h, 0, sp->height() - subRect->y);

						bool retVal = m_subConvertCtx->applyCachedContext(
							subRect->w, subRect->h, AV_PIX_FMT_PAL8,
							subRect->w, subRect->h, AV_PIX_FMT_BGRA,
							0, nullptr, nullptr, nullptr);

						if (!retVal) {
							av_log(nullptr, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
							return;
						}

						if (!SDL_LockTexture(m_subTexture, (SDL_Rect *)subRect, (void**)pixels, pitch)) {
							m_subConvertCtx->scale((const uint8_t * const *)subRect->data, subRect->linesize,
								0, subRect->h, pixels, pitch);
							SDL_UnlockTexture(m_subTexture);
						}
					}
					sp->setUploaded(1);
				}
			}
			else {
				sp = nullptr;
			}
		}
	}

	calculateDisplayRect(&rect, m_xLeft, m_yTop, m_width, m_height, vp->width(), vp->height(), vp->sar());

	if (!vp->uploaded()) {
		int sdlPixFmt = vp->frameFormat() == AV_PIX_FMT_YUV420P ? SDL_PIXELFORMAT_YV12 : SDL_PIXELFORMAT_ABGR8888;		
		if (reallocTexture(&m_vidTexture, sdlPixFmt, vp->frameWidth(), vp->frameHeight(), SDL_BLENDMODE_NONE, 0) < 0) {
			return;
		}
		if (uploadTexture(m_vidTexture, vp->frame()) < 0) {
			return;
		}
		vp->setUploaded(1);
		vp->setFlipV(vp->frameLineSize0() < 0);
	}

	m_renderer->copyEx(*m_vidTexture, rect, 0, (vp->flipV() ? SDL_FLIP_VERTICAL : SDL_FLIP_NONE));

	if (sp) {
#if USE_ONEPASS_SUBTITLE_RENDER
		SDL_RenderCopy(m_renderer, m_subTexture, nullptr, &rect);
#else
		int i;
		double xRatio = (double)rect.w / (double)sp->width();
		double yRatio = (double)rect.h / (double)sp->height();
		for (i = 0; i < sp->sub()->num_rects; i++) {
			SDL_Rect *subRect = (SDL_Rect*)sp->sub()->rects[i];
			SDL_Rect target = { rect.x + subRect->x * xRatio,
				rect.y + subRect->y * yRatio, subRect->w * xRatio, subRect->h * yRatio };
			m_renderer->copy(*m_subTexture, *subRect, target);
		}
#endif
	}
}

void VideoState::displayVideoAudio()
{
	int i, iStart, x, y1, y, ys, delay, n, nbDisplayChannels;
	int ch, channels, h, h2;
	int64_t timeDiff;
	int rdftBits, nbFreq;

	for (rdftBits = 1; (1 << rdftBits) < 2 * m_height; rdftBits++) {
		// nothing to do
	}
	nbFreq = 1 << (rdftBits - 1);
	channels = m_audioTgt.channels;
	nbDisplayChannels = channels;
	if (m_paused) {
		int dataUsed = m_showMode == SHOW_MODE_WAVES ? m_width : (2 * nbFreq);
		n = 2 * channels;
		delay = m_audioWriteBufSize;
		delay /= n;

		if (s_audioCallbackTime) {
			timeDiff = av_gettime_relative() - s_audioCallbackTime;
			delay -= (timeDiff * m_audioTgt.freq) / 1000000;
		}

		delay += 2 * dataUsed;
		if (delay < dataUsed) {
			delay = dataUsed;
		}

		iStart = x = computeMod(m_sampleArrayIndex - delay * channels, SAMPLE_ARRAY_SIZE);
		if (m_showMode == SHOW_MODE_WAVES) {
			h = INT_MIN;
			for (i = 0; i < 1000; i += channels) {
				int idx = (SAMPLE_ARRAY_SIZE + x - i) % SAMPLE_ARRAY_SIZE;
				int a = m_sampleArray[idx];
				int b = m_sampleArray[(idx + 4 * channels) % SAMPLE_ARRAY_SIZE];
				int c = m_sampleArray[(idx + 5 * channels) % SAMPLE_ARRAY_SIZE];
				int d = m_sampleArray[(idx + 9 * channels) % SAMPLE_ARRAY_SIZE];
				int score = a - d;
				if (h < score && (b ^ c) < 0) {
					h = score;
					iStart = idx;
				}
			}
		}
		m_lastIStart = iStart;
	}
	else {
		iStart = m_lastIStart;
	}

	if (m_showMode == SHOW_MODE_WAVES) {
		m_renderer->setDrawColor(255, 255, 255, 255);

		h = m_height / nbDisplayChannels;
		h2 = (h * 9) / 20;
		for (ch = 0; ch < nbDisplayChannels; ch++) {
			i = iStart + ch;
			y1 = m_yTop + ch * h + (h / 2);
			for (x = 0; x < m_width; x++) {
				y = (m_sampleArray[i] * h2) >> 15;
				if (y < 0) {
					y = -y;
					ys = y1 - y;
				}
				else {
					ys = y1;
				}
				fillRectangle(m_xLeft + x, ys, 1, y);
				i += channels;
				if (i >= SAMPLE_ARRAY_SIZE) {
					i -= SAMPLE_ARRAY_SIZE;
				}
			}
		}

		m_renderer->setDrawColor(0, 0, 255, 255);

		for (ch = 1; ch < nbDisplayChannels; ch++) {
			y = m_yTop + ch * h;
			fillRectangle(m_xLeft, y, m_width, 1);
		}
	}
	else {
		if (reallocTexture(&m_visTexture, SDL_PIXELFORMAT_ARGB8888, m_width, m_height, SDL_BLENDMODE_NONE, 1) < 0) {
			return;
		}
		nbDisplayChannels = FFMIN(nbDisplayChannels, 2);
		if (rdftBits != m_rdftBits) {
			av_rdft_end(m_rdft);
			av_free(m_rdftData);
			m_rdft = av_rdft_init(rdftBits, DFT_R2C);
			m_rdftBits = rdftBits;
			m_rdftData = static_cast<FFTSample*>(av_malloc_array(nbFreq, 4 * sizeof(*m_rdftData)));
		}
		if (!m_rdft || !m_rdftData) {
			av_log(nullptr, AV_LOG_ERROR, "Failed to allocate buffers for RDFT, switching to waves display\n");
			m_showMode = SHOW_MODE_WAVES;
		}
		else {
			FFTSample *data[2];
			SDL_Rect rect = { m_xPos, 0, 1, m_height };
			uint32_t *pixels;
			int pitch;
			for (ch = 0; ch < nbDisplayChannels; ch++) {
				data[ch] = m_rdftData + 2 * nbFreq * ch;
				i = iStart + ch;
				for (x = 0; x < 2 * nbFreq * ch; x++) {
					double w = (x - nbFreq) * (1.0 / nbFreq);
					data[ch][x] = m_sampleArray[i] * (1.0 - w*w);
					i += channels;
					if (i >= SAMPLE_ARRAY_SIZE) {
						i -= SAMPLE_ARRAY_SIZE;
					}
				}
				av_rdft_calc(m_rdft, data[ch]);
			}

			if (!SDL_LockTexture(m_visTexture, &rect, (void**)&pixels, &pitch)) {
				pitch >>= 2;
				pixels += pitch * m_height;
				for (y = 0; y < m_height; y++) {
					double w = 1 / sqrt(nbFreq);
					int a = sqrt(w * sqrt(data[0][2 * y + 0] * data[0][2 * y + 0] + data[0][2 * y + 1] * data[0][2 * y + 1]));
					int b = (nbDisplayChannels == 2) ? sqrt(w * hypot(data[1][2 * y + 0], data[1][2 * y + 1])) : a;
					a = FFMIN(a, 255);
					b = FFMIN(b, 255);
					pixels -= pitch;
					*pixels = (a << 16) + (b << 8) + ((a + b) >> 1);
				}
				SDL_UnlockTexture(m_visTexture);
			}
			m_renderer->copy(*m_visTexture);
		}
		if (!m_paused) {
			m_xPos++;
		}
		if (m_xPos >= m_width) {
			m_xPos = m_xLeft;
		}
	}
}

static void printError(const char *filename, int err)
{
	char errBuf[128];
	const char* errBufPtr = errBuf;

	if (av_strerror(err, errBuf, sizeof(errBuf)) < 0) {
		//errBufPtr = strerror(AVUNERROR(err));
		strerror_s(errBuf, sizeof(errBuf), AVUNERROR(err));
	}
	av_log(nullptr, AV_LOG_ERROR, "%s: %s\n", filename, errBufPtr);
}

static AVDictionary **setupFoundStreamInfoOpts(AVFormatContext *s, AVDictionary *codecOpts)
{	
	AVDictionary **opts;

	if (!s->nb_streams) {
		return nullptr;
	}
	opts = static_cast<AVDictionary**>(av_mallocz_array(s->nb_streams, sizeof(*opts)));
	if (!opts) {
		av_log(nullptr, AV_LOG_ERROR, "Could not alloc memory for stream options.\n");
		return nullptr;
	}
	for (unsigned int i = 0; i < s->nb_streams; i++) {
		opts[i] = filterCodecOpts(codecOpts, s->streams[i]->codecpar->codec_id, s, s->streams[i], nullptr);
	}
	return opts;
}

static bool isRealtime(AVFormatContext *ic)
{
	if (!strcmp(ic->iformat->name, "rtp") ||
		!strcmp(ic->iformat->name, "rtsp") ||
		!strcmp(ic->iformat->name, "sdp")) {
		return true;
	}

	if (ic->pb &&
		(!strncmp(ic->filename, "rtp:", 4) ||
			!strncmp(ic->filename, "udp:", 4))) {
		return true;
	}
	return false;
}

int VideoState::runReadStream()
{
	AVFormatContext *ic = nullptr;
	int err, i, ret;
	int stIndex[AVMEDIA_TYPE_NB];
	AVPacket pkt1, *pkt = &pkt1;
	int64_t streamStartTime;
	int pktInPlayRange = 0;
	int scanAllPmtsSet = 0;
	AVDictionaryEntry *t;
	AVDictionary **opts;
	int origNbStreams;
	int64_t pktTs;

	Mutex waitMutex;

	memset(stIndex, -1, sizeof(stIndex));

	ic = avformat_alloc_context();
	if (!ic) {
		av_log(nullptr, AV_LOG_FATAL, "Could not allocate context.\n");
		ret = AVERROR(ENOMEM);
		// TODO : handle error
	}
	ic->interrupt_callback.callback = decodeInterruptCb;
	ic->interrupt_callback.opaque = this;
	if (!av_dict_get(m_formatOpts, "scan_all_pmts", nullptr, AV_DICT_MATCH_CASE)) {
		av_dict_set(&m_formatOpts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
		scanAllPmtsSet = 1;
	}

	err = avformat_open_input(&ic, m_filename, m_iFormat, &m_formatOpts);
	if (err < 0) {
		printError(m_filename, err);
		ret = -1;
		// TOOD : handle error
	}
	if (scanAllPmtsSet) {
		av_dict_set(&m_formatOpts, "scan_all_pmts", nullptr, AV_DICT_MATCH_CASE);
	}

	if ((t = av_dict_get(m_formatOpts, "", nullptr, AV_DICT_IGNORE_SUFFIX))) {
		av_log(nullptr, AV_LOG_ERROR, "Option %s not found.\n", t->key);
		ret = AVERROR_OPTION_NOT_FOUND;
		// TODO : handle error
	}

	m_ic = ic;

	if (m_genPts) {
		ic->flags |= AVFMT_FLAG_GENPTS;
	}

	av_format_inject_global_side_data(ic);

	opts = setupFoundStreamInfoOpts(ic, m_codecOpts);
	origNbStreams = ic->nb_streams;

	err = avformat_find_stream_info(ic, opts);

	for (i = 0; i < origNbStreams; i++) {
		av_dict_free(&opts[i]);
	}
	av_freep(&opts);

	if (err < 0) {
		av_log(nullptr, AV_LOG_WARNING, "%s: could not find codec parameters\n", m_filename);
		ret = -1;
		// TODO : handle error
	}

	if (ic->pb) {
		ic->pb->eof_reached = 0;
	}

	if (m_seekByBytes < 0) {
		m_seekByBytes = !!(ic->iformat->flags & AVFMT_TS_DISCONT) && strcmp("ogg", ic->iformat->name);
	}

	m_maxFrameDuration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;

	if (m_windowTitle && (t = av_dict_get(ic->metadata, "title", NULL, 0))) {
		m_windowTitle = av_asprintf("%s - %s", t->value, m_filename);
	}

	if (m_startTime != AV_NOPTS_VALUE) {
		int64_t timestamp;

		timestamp = m_startTime;

		if (ic->start_time != AV_NOPTS_VALUE) {
			timestamp += ic->start_time;
		}

		ret = avformat_seek_file(ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
		if (ret < 0) {
			av_log(NULL, AV_LOG_WARNING, "%s: could not seek to position %0.3f\n",
				m_filename, (double)timestamp / AV_TIME_BASE);
		}
	}

	// TODO : read function
	m_realtime = isRealtime(ic);

	if (m_showStatus) {
		av_dump_format(ic, 0, m_filename, 0);	// what is stream? 
	}

	for (i = 0; i < ic->nb_streams; i++) {
		AVStream *st = ic->streams[i];
		AVMediaType type = st->codecpar->codec_type;
		st->discard = AVDISCARD_ALL;
		if (type >= 0 && m_wantedStreamSpec[type] && stIndex[type] == -1) {
			if (avformat_match_stream_specifier(ic, st, m_wantedStreamSpec[type]) > 0) {
				stIndex[type] = i;
			}
		}
	}

	for (i = 0; i < AVMEDIA_TYPE_NB; i++) {
		if (m_wantedStreamSpec[i] && stIndex[i] == -1) {
			av_log(nullptr, AV_LOG_ERROR, "Stream specifiler %s does not match any %s stream\n",
				m_wantedStreamSpec[i], av_get_media_type_string(static_cast<AVMediaType>(i)));
			stIndex[i] = INT_MAX;
		}
	}

	if (!m_videoDisable) {
		stIndex[AVMEDIA_TYPE_VIDEO] = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, stIndex[AVMEDIA_TYPE_VIDEO], -1, nullptr, 0);
	}

	if (!m_audioDisable) {
		stIndex[AVMEDIA_TYPE_AUDIO] = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, stIndex[AVMEDIA_TYPE_AUDIO], stIndex[AVMEDIA_TYPE_VIDEO],
			nullptr, 0);
	}

	if (!m_videoDisable && !m_subtitleDisable) {
		stIndex[AVMEDIA_TYPE_SUBTITLE] = av_find_best_stream(ic, AVMEDIA_TYPE_SUBTITLE, stIndex[AVMEDIA_TYPE_SUBTITLE],
			(stIndex[AVMEDIA_TYPE_AUDIO] >= 0 ? stIndex[AVMEDIA_TYPE_AUDIO] : stIndex[AVMEDIA_TYPE_VIDEO]), nullptr, 0);
	}

	m_showMode = s_showMode;

	if (stIndex[AVMEDIA_TYPE_VIDEO] >= 0) {
		AVStream *st = ic->streams[stIndex[AVMEDIA_TYPE_VIDEO]];
		AVCodecParameters *codecpar = st->codecpar;
		AVRational sar = av_guess_sample_aspect_ratio(ic, st, nullptr);
		if (codecpar->width) {
			setDefaultWindowSize(codecpar->width, codecpar->height, sar);
		}
	}

	if (stIndex[AVMEDIA_TYPE_AUDIO] > 0) {
		openStreamComponent(stIndex[AVMEDIA_TYPE_AUDIO]);
	}

	ret = -1;
	if (stIndex[AVMEDIA_TYPE_VIDEO] >= 0) {
		ret = openStreamComponent(stIndex[AVMEDIA_TYPE_VIDEO]);
	}
	if (m_showMode == SHOW_MODE_NONE) {
		m_showMode = ret >= 0 ? SHOW_MODE_VIDEO : SHOW_MODE_RDFT;
	}
	if (stIndex[AVMEDIA_TYPE_SUBTITLE] >= 0) {
		openStreamComponent(stIndex[AVMEDIA_TYPE_SUBTITLE]);
	}

	if (m_videoStream < 0 && m_audioStream < 0) {
		av_log(nullptr, AV_LOG_FATAL, "Failed to open file '%s' or configure filtergraph\n", m_filename);
		ret = -1;
		// TODO : handle exception
	}

	if (s_infiniteBuffer < 0 && m_realtime) {
		s_infiniteBuffer = 1;
	}

	for (;;) {
		if (m_abortRequest) {
			break;
		}
		if (m_paused != m_lastPaused) {
			m_lastPaused = m_paused;
			if (m_paused) {
				m_readPauseReturn = av_read_pause(ic);
			}
			else {
				av_read_play(ic);
			}
		}
#if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
		if (m_paused &&
			(!strcmp(ic->iformat->name, "rtsp") ||
			(ic->pb && !strncmp(inputFilename, "mmsh:", 5))) {
			SDL_Delay(10);
			continue;
		}
#endif
		if (m_seekReq) {
			int64_t seekTarget = m_seekPos;
			int64_t seekMin = m_seekRel > 0 ? seekTarget - m_seekRel + 2 : INT64_MIN;
			int64_t seekMax = m_seekRel < 0 ? seekTarget - m_seekRel - 2 : INT64_MAX;
			// FIXME the +-2 is due to rounding being not done in the correct direction in generation
			//      of the seek_pos/seek_rel variables

			ret = avformat_seek_file(m_ic, -1, seekMin, seekTarget, seekMax, m_seekFlags);
			if (ret < 0) {
				av_log(nullptr, AV_LOG_ERROR, "%s: error while seeking\n", m_ic->filename);
			}
			else {
				if (m_audioStream >= 0) {
					m_audioQ.flush();
					m_audioQ.putFlushPkt();
				}
				if (m_subtitleStream >= 0) {
					m_subtitleQ.flush();
					m_subtitleQ.putFlushPkt();
				}
				if (m_videoStream >= 0) {
					m_videoQ.flush();
					m_videoQ.putFlushPkt();
				}
				if (m_seekFlags & AVSEEK_FLAG_BYTE) {
					m_extClk.setClock(NAN, 0);
				}
				else {
					m_extClk.setClock(seekTarget / (double)AV_TIME_BASE, 0);
				}
			}
			m_seekReq = 0;
			m_queueAttachmentsReq = 1;
			m_eof = 0;
			if (m_paused) {
				stepToNextFrame();
			}
		}

		if (m_queueAttachmentsReq) {
			if (m_videoSt && m_videoSt->disposition & AV_DISPOSITION_ATTACHED_PIC) {
				AVPacket copy;
				if ((ret = av_copy_packet(&copy, &m_videoSt->attached_pic)) < 0) {
					// handle exception
				}
				m_videoQ.put(&copy);
				m_videoQ.putNullPkt(m_videoStream);
			}
			m_queueAttachmentsReq = 0;
		}

		const int MAX_QUEUE_SIZE = 15 * 1024 * 1024;
		//int level = av_log_get_level();
		//av_log_set_level(AV_LOG_DEBUG);
		//av_log(nullptr, AV_LOG_DEBUG, "AQ : %d, VQ : %d, SQ : %d\n", m_audioQ.size(), m_videoQ.size(), m_subtitleQ.size());
		//av_log(nullptr, AV_LOG_DEBUG, "AQE : %d, VQE : %d, SQE : %d\n", m_audioQ.hasEnoughPackets(m_audioSt, m_audioStream), 
		//m_videoQ.hasEnoughPackets(m_videoSt, m_videoStream), 
		//m_subtitleQ.hasEnoughPackets(m_subtitleSt, m_subtitleStream));
		//av_log_set_level(level);
		if (s_infiniteBuffer < 1 &&
			(m_audioQ.size() + m_videoQ.size() + m_subtitleQ.size() > MAX_QUEUE_SIZE ||
			(m_audioQ.hasEnoughPackets(m_audioSt, m_audioStream) &&
				m_videoQ.hasEnoughPackets(m_videoSt, m_videoStream) &&
				m_subtitleQ.hasEnoughPackets(m_subtitleSt, m_subtitleStream)))) {

			waitMutex.lock();
			m_condReadThread->waitTimeout(waitMutex, 10);
			waitMutex.unlock();
			continue;
		}

		if (!m_paused &&
			(!m_audioSt || (m_audDec->finished() == m_audioQ.serial() && m_sampleQ.remaining() == 0)) &&
			(!m_videoSt || (m_vidDec->finished() == m_videoQ.serial() && m_pictureQ.remaining() == 0))) {
			if (s_loop != 1 && (!s_loop || --s_loop)) {
				seekStream(m_startTime != AV_NOPTS_VALUE ? m_startTime : 0, 0, 0);
			}
			else if (s_autoexit) {
				ret = AVERROR_EOF;
				// handle exception
			}
		}
		ret = av_read_frame(ic, pkt);
		if (ret < 0) {
			if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !m_eof) {
				if (m_videoStream >= 0) {
					m_videoQ.putNullPkt(m_videoStream);
				}
				if (m_audioStream >= 0) {
					m_audioQ.putNullPkt(m_audioStream);
				}
				if (m_subtitleStream >= 0) {
					m_subtitleQ.putNullPkt(m_subtitleStream);
				}
				m_eof = 1;
			}
			if (ic->pb && ic->pb->error) {
				break;
			}
			waitMutex.lock();
			m_condReadThread->waitTimeout(waitMutex, 10);
			waitMutex.unlock();
		}
		else {
			m_eof = 0;
		}

		streamStartTime = ic->streams[pkt->stream_index]->start_time;
		pktTs = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
		pktInPlayRange = s_duration == AV_NOPTS_VALUE ||
			(pktTs - (streamStartTime != AV_NOPTS_VALUE ? streamStartTime : 0)) * av_q2d(ic->streams[pkt->stream_index]->time_base) -
			(double)(m_startTime != AV_NOPTS_VALUE ? m_startTime : 0) / 1000000 <= ((double)s_duration / 1000000);
		if (pkt->stream_index == m_audioStream && pktInPlayRange) {
			m_audioQ.put(pkt);
		}
		else if (pkt->stream_index == m_videoStream && pktInPlayRange && !(m_videoSt->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
			m_videoQ.put(pkt);
		}
		else if (pkt->stream_index == m_subtitleStream && pktInPlayRange) {
			m_subtitleQ.put(pkt);
		}
		else {
			av_packet_unref(pkt);
		}
	}

	ret = 0;

fail:
	if (ic && !m_ic) {
		avformat_close_input(&ic);
	}

	if (ret != 0) {
		SDL_Event event;

		event.type = FF_QUIT_EVENT;
		event.user.data1 = this;
		SDL_PushEvent(&event);
	}

	//SDL_DestroyMutex(waitMutex);
	return 0;
}

int VideoState::readThread(void * arg)
{
	VideoState *is = static_cast<VideoState *>(arg);
	return is->runReadStream();
}

int VideoState::decodeInterruptCb(void * ctx)
{
	VideoState *is = static_cast<VideoState*>(ctx);
	return is->m_abortRequest;
}

void VideoState::handleAudioCallback(Uint8 *stream, int len)
{
	int audioSize, len1;

	m_audioCallbackTime = av_gettime_relative();

	while (len > 0) {
		if (m_audioBufIndex >= m_audioBufSize) {
			audioSize = audioDecodeFrame();
			if (audioSize < 0) {
				m_audioBuf = nullptr;
				m_audioBufSize = SDL_AUDIO_MIN_BUFFER_SIZE / m_audioTgt.frameSize * m_audioTgt.frameSize;
			}
			else {
				if (m_showMode != SHOW_MODE_VIDEO) {
					updateSampleDisplay((int16_t *)m_audioBuf, audioSize);
				}
				m_audioBufSize = audioSize;
			}
			m_audioBufIndex = 0;
		}
		len1 = m_audioBufSize - m_audioBufIndex;
		if (len1 > len) {
			len1 = len;
		}
		if (!m_muted && m_audioBuf && m_audioVolume == SDL_MIX_MAXVOLUME) {
			memcpy(stream, (uint8_t *)m_audioBuf + m_audioBufIndex, len1);
		}
		else {
			memset(stream, 0, len1);
			if (!m_muted && m_audioBuf) {
				SDL_MixAudio(stream, (uint8_t *)m_audioBuf + m_audioBufIndex, len1, m_audioVolume);
			}
		}
		len -= len1;
		stream += len1;
		m_audioBufIndex += len1;
	}
	m_audioWriteBufSize = m_audioBufSize - m_audioBufIndex;

	if (!isnan(m_audioClock)) {
		setClockAt(m_audClk, m_audioClock - (double)(2 * m_audioHwBufSize + m_audioWriteBufSize) / m_audioTgt.bytesPerSec,
			m_audioClockSerial, m_audioCallbackTime / 1000000.0);
		syncClockToSlave(m_extClk, m_audClk);
	}
}

void VideoState::sdlAudioCallback(void * opaque, Uint8 * stream, int len)
{
	VideoState *is = static_cast<VideoState *>(opaque);
	is->handleAudioCallback(stream, len);
}

int VideoState::runAudioDecoding()
{
	AVFrame *frame = av_frame_alloc();
	Frame *af;
#if CONFIG_AVFILTER
	int lastSerial = -1;
	int64_t decChannelLayout;
	int reconfigure;
#endif
	int gotFrame = 0;
	AVRational tb;
	int ret = 0;

	if (!frame) {
		return AVERROR(ENOMEM);
	}

	do {
		if ((gotFrame = m_audDec->decodeFrame(frame, nullptr)) < 0) {
			// TODO : throw exception
		}

		if (gotFrame) {
			tb = AVRational{ 1, frame->sample_rate };
#if CONFIG_AVFILTER
			decChannelLayout = getValidChannelLayout(frame->channel_layout, frame->channels);
			reconfigure = cmpAudioFmts(m_audioFilterSrc.fmt, m_audioFilterSrc.channels, frame->format, frame->channels) ||
				m_audioFilterSrc.channelLayout != decChannelLayout ||
				m_audioFilterSrc.freq != frame->sample_rate ||
				m_audDec.pktSerial != lastSerial;

			if (reconfigure) {
				char buf1[1024], buf2[1024];
				av_get_channel_layout_string(buf1, sizeof(buf1), -1, m_audioFilterSrc.channelLayout);
				av_get_channel_layout_string(buf2, sizeof(buf2), -1, decChannelLayout);
				av_log(nullptr, AV_LOG_DEBUG,
					"Audio frame changed from rate:%d ch:%d fmt:%s layout:%s serial:%d to rate:%d ch:%d fmt:%s layout:%s serial:%d\n",
					m_audioFilterSrc.freq, m_audioFilterSrc.channels, av_get_sample_fmt_name(m_audioFilterSrc.fmt), buf1, lastSerial,
					frame->sample_rate, frame->channels, av_get_sample_fmt_name(frame->format), buf2, m_audDec.pktSerial);

				m_audioFilterSrc.fmt = frame->format;
				m_audioFilterSrc.channels = frame->channels;
				m_audioFilterSrc.channelLayout = decChannelLayout;
				m_audioFilterSrc.freq = frame->sample_rate;
				lastSerial = m_audDec.pktSerial;

				if ((ret = is.configureAudioFilters(afilters, 1)) < 0) {
					// TODO : throw exception;
				}
			}

			if ((ret = av_buffersrc_add_frame(m_inAudioFilter, frame)) < 0) {
				// TODO : throw exception
			}

			while ((ret = av_buffersink_get_frame_flags(m_outAudioFilter, frame, 0)) >= 0) {
				tb = av_buffersink_get_time_base(m_outAudioFilter);
#endif
				if (!(af = m_sampleQ.peekWritable())) {
					// TODO : throw exception
				}

				af->setPosInfo((frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb),
					frame->pkt_pos, m_audDec->pktSerial(),
					av_q2d(AVRational{ frame->nb_samples, frame->sample_rate }));
				af->moveRef(frame);
				m_sampleQ.push();

#if CONFIG_AVFILTER
			}
			if (ret == AVERROR_EOF) {
				m_audDec.finished = audDec.pktSerial;
			}
#endif
		}
	} while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);

the_end:
#if CONFIG_AVFILTER
	avfilter_graph_free(&m_agraph);
#endif
	av_frame_free(&frame);
	return ret;
}

int VideoState::audioThread(void * arg)
{
	VideoState *is = static_cast<VideoState *>(arg);
	return is->runAudioDecoding();
}

int VideoState::runVideoDecoding()
{
	AVFrame *frame = av_frame_alloc();
	double pts;
	double duration;
	int ret;
	AVRational tb = m_videoSt->time_base;
	AVRational frameRate = av_guess_frame_rate(m_ic, m_videoSt, nullptr);

#if CONFIG_AVFILTER
	AVFilterGraph *graph = avfilter_graph_alloc();
	AVFilterContext *filtOut = nullptr, *filtIn = nullptr;
	int lastW = 0;
	int lastH = 0;
	AVPixelFormat lastFormat = -2;
	int lastSerial = -1;
	int lastVFilterIdx = 0;
	if (!graph) {
		av_frame_free(&frame);
		return AVERROR(ENOMEM);
	}
#endif

	if (!frame) {
#if CONFIG_AVFILTER
		avfilter_graph_free(&graph);
#endif
		return AVERROR(ENOMEM);
	}

	for (;;) {
		ret = getVideoFrame(frame);
		if (ret < 0) {
			// FIXME : should not use goto
			goto the_end;
		}
		if (!ret) {
			continue;
		}

#if CONFIG_AVFILTER
		if (lastW != frame->width ||
			lastH != frame->height ||
			lastFormat != frame->format ||
			lastSerial != m_vidDec.pktSerial() ||
			lastVFilterIdx != m_vFilterIdx) {
			av_log(nullptr, AV_LOG_DEBUG,
				"Video frame changed from size:%dx%d format:%s serial:%d to size:%dx%d format:%s serial:%d\n",
				lastW, lastH,
				(const char*)av_x_if_null(av_get_pix_fmt_name(lastFormat), "none"), lastSerial,
				frame->width, frame->height,
				(const char*)av_x_if_null(av_get_pix_fmt_name(frame->format), "none"), m_vidDec.pktSerial());
			// TODO : implement filter job.
		}
#endif
		duration = (frameRate.num && frameRate.den ? av_q2d(AVRational{ frameRate.den, frameRate.num }) : 0);
		pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
		ret = queuePicture(frame, pts, duration, av_frame_get_pkt_pos(frame), m_vidDec->pktSerial());
		av_frame_unref(frame);
#if CONFIG_AVFILTER
	}
#endif
	if (ret < 0) {
		// FIXME: should not use goto
		goto the_end;
	}
}
the_end:
#if CONFIG_AVFILTER
avfilter_graph_free(&graph);
#endif
av_frame_free(&frame);
return 0;
}

int VideoState::videoThread(void * arg)
{
	VideoState *is = static_cast<VideoState*>(arg);
	return is->runVideoDecoding();
}

int VideoState::runSubtitleDecoding()
{
	Frame *sp;
	int gotSubtitle;
	double pts;

	for (;;) {
		if (!(sp = m_subPictureQ.peekWritable())) {
			return 0;
		}
		if ((gotSubtitle = m_subDec->decodeFrame(nullptr, sp->sub())) < 0) {
			break;
		}

		pts = 0;

		if (gotSubtitle && sp->subFormat() == 0) {
			if (sp->subPts() != AV_NOPTS_VALUE) {
				pts = sp->subPts() / (double)AV_TIME_BASE;
			}
			sp->setPts(pts);
			sp->setSerial(m_subDec->pktSerial());
			sp->setAreaInfo(m_subDec->avctx()->width,
				m_subDec->avctx()->height);
			sp->setUploaded(0);
			m_subPictureQ.push();
		}
		else if (gotSubtitle) {
			avsubtitle_free(sp->sub());
		}
	}
	return 0;
}

int VideoState::subTitleThread(void * arg)
{
	VideoState *is = static_cast<VideoState*>(arg);
	return is->runSubtitleDecoding();
}