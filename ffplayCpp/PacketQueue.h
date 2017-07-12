#pragma once

extern "C"	{
#include <libavformat/avformat.h>
}
#include <SDL.h>
#include <memory>

class Condition;
class Mutex;

struct MyAVPacketList
{
	AVPacket pkt;
	struct MyAVPacketList *next;
	int serial;
};

class PacketQueue
{
public:
	PacketQueue();
	~PacketQueue();

public:
	int putPrivate(AVPacket *pkt);
	bool isAbortRequested();
	const int& serial() const { return m_serial; }
	void start();
	int nbPackets() const { return m_nbPackets; }
	int get(AVPacket *pkt, int block, int *serial);
	void flush();
	int put(AVPacket *pkt);
	int putFlushPkt();
	int putNullPkt(int streamIndex);
	int size() { return m_size; }
	int hasEnoughPackets(AVStream *st, int streamId);
	static bool isFlushData(uint8_t* &data);

private:
	MyAVPacketList *m_firstPkt = nullptr;
	MyAVPacketList *m_lastPkt = nullptr;
	int m_nbPackets = 0;
	int m_size = 0;
	int64_t m_duration = 0;
	int m_abortRequest = 0;
	int m_serial = 0;
	std::unique_ptr<Mutex> m_mutex;
	std::unique_ptr<Condition> m_cond;
	static AVPacket s_flushPkt;
};

