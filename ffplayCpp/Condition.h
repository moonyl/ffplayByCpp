#pragma once

#include <memory>
#include <SDL.h>

class Mutex;

class Condition
{
public:
	Condition();
	~Condition();

public:
	int signal();
	int wait(Mutex &mutex);
	int waitTimeout(Mutex &mutex, Uint32 milisec);

private:
	struct SDLConditionDestroyer
	{
		void operator()(SDL_cond* c) const
		{
			SDL_DestroyCond(c);
		}
	};

private:
	std::unique_ptr<SDL_cond, SDLConditionDestroyer> m_condition;
};

