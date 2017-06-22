#include "FrameQueue.h"
#include <SDL.h>
#include "PacketQueue.h"

void Frame::unref()
{
	av_frame_unref(m_frame);
	avsubtitle_free(&m_sub);
}

void Frame::moveRef(AVFrame * frame)
{
	av_frame_move_ref(m_frame, frame);
}

FrameQueue::FrameQueue(PacketQueue & pktQ, int maxSize, int keepLast) :
	m_pktQ(pktQ)
{
	if (!(m_mutex = SDL_CreateMutex())) {
		av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
		// TODO : throw exception
	}
	if (!(m_cond = SDL_CreateCond())) {
		av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
		// TODO : throw exception
	}

	m_maxSize = FFMIN(maxSize, FRAME_QUEUE_SIZE);
	m_keepLast = !!keepLast;
}

FrameQueue::~FrameQueue()
{
	SDL_DestroyMutex(m_mutex);
	SDL_DestroyCond(m_cond);
}

void FrameQueue::signal()
{
	SDL_LockMutex(m_mutex);
	SDL_CondSignal(m_cond);
	SDL_UnlockMutex(m_mutex);
}

Frame * FrameQueue::peek()
{	
	return &m_queue[(m_rIndex + m_rIndexShown) % m_maxSize];
}

Frame * FrameQueue::peekNext()
{
	return &m_queue[(m_rIndex + m_rIndexShown + 1) % m_maxSize];
}

Frame * FrameQueue::peekLast()
{
	return &m_queue[m_rIndex];
}

Frame * FrameQueue::peekWritable()
{
	SDL_LockMutex(m_mutex);
	while (m_size >= m_maxSize && !m_pktQ.isAbortRequested()) {
		SDL_CondWait(m_cond, m_mutex);
	}
	SDL_UnlockMutex(m_mutex);

	if (m_pktQ.isAbortRequested()) {
		return nullptr;
	}

	return &m_queue[m_wIndex];
}

Frame * FrameQueue::peekReadable()
{
	SDL_LockMutex(m_mutex);
	while (m_size - m_rIndexShown <= 0 && !m_pktQ.isAbortRequested()) {
		SDL_CondWait(m_cond, m_mutex);
	}
	SDL_UnlockMutex(m_mutex);

	if (m_pktQ.isAbortRequested()) {
		return nullptr;
	}

	return &m_queue[(m_rIndex + m_rIndexShown) % m_maxSize];
}

void FrameQueue::push()
{
	if (++m_wIndex == m_maxSize) {
		m_wIndex = 0;
	}
	SDL_LockMutex(m_mutex);
	m_size++;
	SDL_CondSignal(m_cond);
	SDL_UnlockMutex(m_mutex);
}

void FrameQueue::next()
{
	if (m_keepLast && !m_rIndexShown) {
		m_rIndexShown = 1;
		return;
	}
	m_queue[m_rIndex].unref();
	if (++m_rIndex == m_maxSize) {
		m_rIndex = 0;
	}
	SDL_LockMutex(m_mutex);
	m_size--;
	SDL_CondSignal(m_cond);
	SDL_UnlockMutex(m_mutex);
}

int FrameQueue::rIndexShown() const
{
	return m_rIndexShown;
}

SDL_mutex * FrameQueue::mutex() const
{
	return m_mutex;
}

int FrameQueue::remaining() const
{
	return m_size - m_rIndexShown;
}

int64_t FrameQueue::lastPos() const
{
	const Frame &f = m_queue[m_rIndex];
	if (m_rIndexShown && f.serial() == m_pktQ.serial()) {
		return f.pos();
	}
	else {
		return -1;
	}
}


