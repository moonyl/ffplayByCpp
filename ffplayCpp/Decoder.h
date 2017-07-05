#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <functional>
#include <memory>
class PacketQueue;
class Thread;
class Condition;

class Decoder
{
public:
	Decoder(AVCodecContext* avctx, PacketQueue &queue, Condition &emptyQueueCond);
	~Decoder();

public:
	int start(int (*func)(void*), void *arg);
	void setStartPts(int64_t startPts);
	void setStartPtsTb(const AVRational &startPtsTb);
	int pktSerial() { return m_pktSerial; }
	int decodeFrame(AVFrame *frame, AVSubtitle *sub);
	int finished() const { return m_finished; }
	AVCodecContext *avctx() const { return m_avctx; }

private:
	AVPacket m_pkt;
	AVPacket m_pktTemp;
	PacketQueue &m_queue;
	AVCodecContext* m_avctx;
	int m_pktSerial = -1;
	int m_finished = 0;
	int m_packetPending = 0;
	Condition &m_emptyQueueCond;
	int64_t m_startPts = AV_NOPTS_VALUE;
	AVRational m_startPtsTb = { 0, 0 };
	int64_t m_nextPts = 0;
	AVRational m_nextPtsTb = { 0, 0 };
	std::unique_ptr<Thread> m_decoderThread;
};

