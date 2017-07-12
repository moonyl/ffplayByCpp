#include "Condition.h"
#include "Mutex.h"


Condition::Condition() :
	m_condition(SDL_CreateCond())
{
}


Condition::~Condition()
{
}

int Condition::signal()
{
	return SDL_CondSignal(m_condition.get());
}

int Condition::wait(Mutex & mutex)
{
	return SDL_CondWait(m_condition.get(), mutex.sdlMutex());
}

int Condition::waitTimeout(Mutex & mutex, Uint32 milisec)
{
	return SDL_CondWaitTimeout(m_condition.get(), mutex.sdlMutex(), milisec);
}
