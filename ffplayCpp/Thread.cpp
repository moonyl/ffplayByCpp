#include "Thread.h"


Thread::Thread(SDL_ThreadFunction func, const char *threadName, void *arg) :
	m_thread(SDL_CreateThread(func, threadName, arg))
{
}


Thread::~Thread()
{
}
