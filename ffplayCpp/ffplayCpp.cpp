// ffplayCpp.cpp : Defines the entry point for the console application.
//

//#include "stdafx.h"
#ifdef _WIN32
#include <windows.h>
#endif

#include <signal.h>

extern "C" {
#include <libavutil/opt.h>
#include <libavdevice/avdevice.h>
#include <libavfilter/avfilter.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <SDL.h>
//#undef main
#include "FfplayCpp.h"
#include "VideoState.h"
#include <thread>
#include <chrono>

FfPlayCpp::FfPlayCpp()
{
	init();
}

FfPlayCpp::~FfPlayCpp()
{
	cleanup();
}

void FfPlayCpp::openStream(const char * inputFile, AVInputFormat * inputFormat)
{
	m_videoState = new VideoState(inputFile, inputFormat);
}

void FfPlayCpp::eventLoop()
{
	SDL_Event event;
	double incr, pos, frac;

	for (;;) {
		double x;
		m_videoState->refreshLoopWaitEvent(event);
		switch (event.type) {
		case SDL_QUIT:
		//case FF_QUIT_EVENT:
			doExit(m_videoState);
			break;
		default:
			break;
		}
	}
}

void FfPlayCpp::init()
{
#ifdef _WIN32
	/* Calling SetDllDirectory with the empty string (but not NULL) removes the
	* current working directory from the DLL search path as a security pre-caution. */
	SetDllDirectory(L"");
#endif
	//"Last message repeated x times" messages
	av_log_set_flags(AV_LOG_SKIP_REPEATED);

	avdevice_register_all();
	avfilter_register_all();
	av_register_all();
	avformat_network_init();

	signal(SIGINT, handleSigTerm);
	signal(SIGTERM, handleSigTerm);

	initSDL();
	initAv();
}

void FfPlayCpp::cleanup()
{
#ifdef _WIN32
	/* Calling SetDllDirectory with the empty string (but not NULL) removes the
	* current working directory from the DLL search path as a security pre-caution. */
	SetDllDirectory(nullptr);
#endif
}

void FfPlayCpp::initSDL()
{
	int flags;
	flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;

	if (!SDL_getenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE")) {
		SDL_setenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE", "1", 1);
	}

	if (SDL_Init(flags)) {
		av_log(NULL, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
		av_log(NULL, AV_LOG_FATAL, "(Did you see the DISPLAY variable?)\n");
		exit(1);
	}

	SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
	SDL_EventState(SDL_USEREVENT, SDL_IGNORE);
}

void FfPlayCpp::initAv()
{
	if (av_lockmgr_register(lockmgr)) {
		av_log(NULL, AV_LOG_FATAL, "Could not initialize lock manager!\n");
		// TODO
		doExit(NULL);
	}
}

void FfPlayCpp::doExit(void *is/*VideoState * is*/)
{
	//if (is) {
		//stream_close(is);
	//}

	//if (renderer) {
	//	SDL_DestroyRenderer(renderer);
	//}

	//if (window) {
	//	SDL_DestroyWindow(window);
	//}

	av_lockmgr_register(NULL);
	//uninit_opts();
	///av_freep(&vfilters_list);
	avformat_network_deinit();
	
	SDL_Quit();
	av_log(NULL, AV_LOG_QUIET, "%s", "");
	exit(0);
}

int FfPlayCpp::lockmgr(void ** mtx, AVLockOp op)
{
	switch (op) {
	case AV_LOCK_CREATE:
		*mtx = SDL_CreateMutex();
		if (!*mtx) {
			av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
			return 1;
		}
		return 0;
	case AV_LOCK_OBTAIN:
		return !!SDL_LockMutex(static_cast<SDL_mutex *>(*mtx));
	case AV_LOCK_RELEASE:
		return !!SDL_UnlockMutex(static_cast<SDL_mutex *>(*mtx));
	case AV_LOCK_DESTROY:
		SDL_DestroyMutex(static_cast<SDL_mutex *>(*mtx));
		return 0;
	}
	return 1;
}

void FfPlayCpp::handleSigTerm(int sig)
{
	exit(123);
}

static void show_help_demuxer()
{
	AVInputFormat *fmt = NULL;
	while ((fmt = av_iformat_next(fmt)))
		printf("name : %s\n", fmt->name);
}

int main(int argc, char *args[])
{
	FfPlayCpp app;

	//const char* inputFile = "D:/video/I_ll_Be_Yours.mp4";
	const char* inputFile = "http://169.56.73.204/hls/test.m3u8";
	AVInputFormat *inputFormat = NULL;
	app.openStream(inputFile, inputFormat);
	//show_help_demuxer();

	app.eventLoop();
#if 0
	while (true) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}
#endif
	return 0;
}
