#include "PacketQueue.h"
extern "C" {
#include <libavutil/avutil.h>
}
#include "Condition.h"
#include "Mutex.h"

AVPacket PacketQueue::s_flushPkt;

PacketQueue::PacketQueue() :
	m_mutex(std::make_unique<Mutex>()),
	m_cond(std::make_unique<Condition>())
{
	// TODO : thread-safe
	if (s_flushPkt.data == nullptr) {
		av_init_packet(&s_flushPkt);
		s_flushPkt.data = (uint8_t *)&s_flushPkt;
	}

	if (!m_mutex) {
		av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
		// TODO : throw exception
	}
	
	if (!m_cond) {
		av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
		// TODO : throw exception
	}

	m_abortRequest = 1;	
}


PacketQueue::~PacketQueue()
{
}

static AVPacket flushPkt;

int PacketQueue::putPrivate(AVPacket * pkt)
{
	MyAVPacketList *pktList;

	if (m_abortRequest) {
		return -1;
	}

	pktList = static_cast<MyAVPacketList*>(av_malloc(sizeof(MyAVPacketList)));
	if (!pktList) {
		return -1;
	}
	// is copy?
	pktList->pkt = *pkt;
	pktList->next = NULL;
	if (pkt == &flushPkt) {
		m_serial++;
	}
	pktList->serial = m_serial;

	if (!m_lastPkt) {
		m_firstPkt = pktList;
	}
	else {
		m_lastPkt->next = pktList;
	}
	m_lastPkt = pktList;
	m_nbPackets++;
	m_size += pktList->pkt.size + sizeof(*pktList);
	m_duration += pktList->pkt.duration;

	m_cond->signal();
	return 0;
}

bool PacketQueue::isAbortRequested()
{
	return !!m_abortRequest;
}

void PacketQueue::start()
{
	m_mutex->lock();
	m_abortRequest = 0;
	putPrivate(&s_flushPkt);
	m_mutex->unlock();
}

int PacketQueue::get(AVPacket * pkt, int block, int * serial)
{
	MyAVPacketList *pkt1;
	int ret;

	m_mutex->lock();

	for (;;) {
		if (m_abortRequest) {
			ret = -1;
			break;
		}

		pkt1 = m_firstPkt;
		if (pkt1) {
			m_firstPkt = pkt1->next;
			if (!m_firstPkt) {
				m_lastPkt = nullptr;
			}
			m_nbPackets--;
			m_size -= pkt1->pkt.size + sizeof(*pkt1);
			m_duration -= pkt1->pkt.duration;
			*pkt = pkt1->pkt;
			if (serial) {
				*serial = pkt1->serial;
			}
			av_free(pkt1);
			ret = 1;
			break;
		}
		else if (!block) {
			ret = 0;
			break;
		}
		else {
			m_cond->wait(*m_mutex);
		}
	}
	m_mutex->unlock();
	return ret;
}

void PacketQueue::flush()
{
	MyAVPacketList *pkt, *pkt1;

	m_mutex->lock();
	for (pkt = m_firstPkt; pkt; pkt = pkt1) {
		pkt1 = pkt->next;
		av_packet_unref(&pkt->pkt);
		av_freep(&pkt);
	}
	m_lastPkt = nullptr;
	m_firstPkt = nullptr;
	m_nbPackets = 0;
	m_size = 0;
	m_duration = 0;

	m_mutex->unlock();
}

int PacketQueue::put(AVPacket * pkt)
{
	int ret;

	m_mutex->lock();
	ret = putPrivate(pkt);
	m_mutex->unlock();

	if (pkt != &flushPkt && ret < 0) {
		av_packet_unref(pkt);
	}

	return ret;
}

int PacketQueue::putFlushPkt()
{
	return put(&s_flushPkt);
}

int PacketQueue::putNullPkt(int streamIndex)
{
	AVPacket pkt1, *pkt = &pkt1;
	av_init_packet(pkt);
	pkt->data = nullptr;
	pkt->size = 0;
	pkt->stream_index = streamIndex;
	return put(pkt);
}

int PacketQueue::hasEnoughPackets(AVStream * st, int streamId)
{
	const int MIN_FRAMES = 25;
	return streamId < 0 || 
		m_abortRequest ||
		(st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
		m_nbPackets > MIN_FRAMES && 
		(!m_duration || av_q2d(st->time_base) * m_duration > 1.0);
}

bool PacketQueue::isFlushData(uint8_t *& data)
{
	if (data == s_flushPkt.data) {
		return true;
	}
	return false;
}
