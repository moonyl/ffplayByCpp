#include "Decoder.h"
#include <SDL.h>
#include "PacketQueue.h"
#include "Thread.h"
#include "Condition.h"

Decoder::Decoder(AVCodecContext* avctx, PacketQueue &queue, Condition &emptyQueueCond) :
	m_avctx(avctx),
	m_queue(queue),
	m_emptyQueueCond(emptyQueueCond)
{
	memset(&m_pkt, sizeof(AVPacket), 0);
}


Decoder::~Decoder()
{
}

int Decoder::start(int (*func)(void*), void * arg)
{
	m_queue.start();
	m_decoderThread = std::make_unique<Thread>(func, "decoder", arg);
	if (!m_decoderThread) {
		av_log(nullptr, AV_LOG_ERROR, "SDL_CreateThread(): %s\n", SDL_GetError());
		return AVERROR(ENOMEM);
	}
	return 0;
}

void Decoder::setStartPts(int64_t startPts)
{
	m_startPts = startPts;
}

void Decoder::setStartPtsTb(const AVRational & startPtsTb)
{
	m_startPtsTb = startPtsTb;
}

static int s_decoderReorderPts = -1;

int Decoder::decodeFrame(AVFrame * frame, AVSubtitle * sub)
{
	int ret = AVERROR(EAGAIN);

	for (;;) {
		AVPacket pkt;

		if (m_queue.isSameSerial(m_pktSerial)) {
			do {
				if (m_queue.isAbortRequested()) {
					return -1;
				}

				// TODO : maybe COR or strategy
				switch (m_avctx->codec_type) {
				case AVMEDIA_TYPE_VIDEO:
					ret = avcodec_receive_frame(m_avctx, frame);
					if (ret >= 0) {
						if (s_decoderReorderPts == -1) {
							frame->pts = frame->best_effort_timestamp;
						}
						else if (!s_decoderReorderPts) {
							frame->pts = frame->pkt_dts;
						}
					}
					break;
				case AVMEDIA_TYPE_AUDIO:
					ret = avcodec_receive_frame(m_avctx, frame);
					if (ret >= 0) {
						AVRational tb = AVRational{ 1, frame->sample_rate };
						if (frame->pts != AV_NOPTS_VALUE) {
							frame->pts = av_rescale_q(frame->pts, av_codec_get_pkt_timebase(m_avctx), tb);
						}
						else if (m_nextPts != AV_NOPTS_VALUE) {
							frame->pts = av_rescale_q(m_nextPts, m_nextPtsTb, tb);
						}
						if (frame->pts != AV_NOPTS_VALUE) {
							m_nextPts = frame->pts + frame->nb_samples;
							m_nextPtsTb = tb;
						}
					}
					break;
				}
				if (ret == AVERROR_EOF) {
					m_finished = m_pktSerial;
					avcodec_flush_buffers(m_avctx);
					return 0;
				}

				if (ret >= 0) {
					return 1;
				}
			} while (ret != AVERROR(EAGAIN));
		}

		do {
			if (m_queue.nbPackets() == 0) {
				m_emptyQueueCond.signal();
			}
			if (m_packetPending) {
				av_packet_move_ref(&pkt, &m_pkt);
				m_packetPending = 0;
			}
			else {
				if (m_queue.get(&pkt, 1, &m_pktSerial) < 0) {
					return -1;
				}
			}
		} while (!m_queue.isSameSerial(m_pktSerial));

		if (PacketQueue::isFlushData(pkt.data)) {
			avcodec_flush_buffers(m_avctx);
			m_finished = 0;
			m_nextPts = m_startPts;
			m_nextPtsTb = m_startPtsTb;
		}
		else {
			if (m_avctx->codec_type == AVMEDIA_TYPE_SUBTITLE) {
				int gotFrame = 0;
				ret = avcodec_decode_subtitle2(m_avctx, sub, &gotFrame, &pkt);
				if (ret < 0) {
					ret = AVERROR(EAGAIN);
				}
				else {
					if (gotFrame && !pkt.data) {
						m_packetPending = 1;
						av_packet_move_ref(&m_pkt, &pkt);
					}
					ret = gotFrame ? 0 : (pkt.data ? AVERROR(EAGAIN) : AVERROR_EOF);
				}
			}
			else {
				if (avcodec_send_packet(m_avctx, &pkt) == AVERROR(EAGAIN)) {
					av_log(m_avctx, AV_LOG_ERROR, "Receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");
					m_packetPending = 1;
					av_packet_move_ref(&m_pkt, &pkt);
				}
			}
			av_packet_unref(&pkt);
		}
	}
	return 0;
}
