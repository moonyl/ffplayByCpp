#pragma once

#include <memory>
#include <SDL.h>

class Condition
{
public:
	Condition();
	~Condition();

public:
	int signal();
	int wait(SDL_mutex &mutex);
	int waitTimeout(SDL_mutex &mutex, Uint32 milisec);

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

