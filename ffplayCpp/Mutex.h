#pragma once

#include <memory>
#include <SDL_mutex.h>

class Mutex
{
public:
	Mutex();
	~Mutex();

public:
	void lock();
	void unlock();

private:
	SDL_mutex *sdlMutex() const;
	friend class Condition;

private:
	struct SDLMutexDestroyer
	{
		void operator()(SDL_mutex* m) const
		{
			SDL_DestroyMutex(m);
		}
	};

private:
	std::unique_ptr<SDL_mutex, SDLMutexDestroyer> m_mutex;
};

