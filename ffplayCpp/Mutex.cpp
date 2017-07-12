#include "Mutex.h"
extern "C" {
#include <libavutil/opt.h>
}

Mutex::Mutex() :
	m_mutex(SDL_CreateMutex())
{
	if (!m_mutex) {
		av_log(nullptr, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
		// TODO : handle error
	}
}


Mutex::~Mutex()
{
}

void Mutex::lock()
{
	SDL_LockMutex(m_mutex.get());
}

void Mutex::unlock()
{
	SDL_UnlockMutex(m_mutex.get());
}

SDL_mutex * Mutex::sdlMutex() const
{
	return m_mutex.get();
}
