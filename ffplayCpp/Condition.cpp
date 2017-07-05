#include "Condition.h"



Condition::Condition() :
	m_condition(SDL_CreateCond())
{
	int a = 0;
}


Condition::~Condition()
{
}

int Condition::signal()
{
	return SDL_CondSignal(m_condition.get());
}

int Condition::wait(SDL_mutex & mutex)
{
	return SDL_CondWait(m_condition.get(), &mutex);
}

int Condition::waitTimeout(SDL_mutex & mutex, Uint32 milisec)
{
	return SDL_CondWaitTimeout(m_condition.get(), &mutex, milisec);
}
