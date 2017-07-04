#pragma once

#include <memory>
#include <SDL.h>

class Thread
{
public:
	Thread(SDL_ThreadFunction func, const char *threadName, void *arg);
	~Thread();

private:
	struct SDLThreadDestroyer
	{
		void operator()(SDL_Thread* t) const
		{
			SDL_WaitThread(t, nullptr);
		}
	};

private:
	std::unique_ptr<SDL_Thread, SDLThreadDestroyer> m_thread;	
};

