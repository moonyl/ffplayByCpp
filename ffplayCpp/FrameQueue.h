#pragma once

extern "C" {
#include <libavformat/avformat.h>
}
#include <memory>
class Mutex;
class PacketQueue;
class Condition;

class Frame
{
public:
	Frame() { 
		m_frame = av_frame_alloc(); // TODO : throw exception 
	}

	~Frame() {
		unref();
		av_frame_free(&m_frame);
	}

public:
	void unref();
	int serial() const { return m_serial; }
	int64_t pos() const { return m_pos; }

	int channels() const { return m_frame->channels; }
	int nbSamples() const { return m_frame->nb_samples; }
	AVSampleFormat frameFormat() const { return static_cast<AVSampleFormat>(m_frame->format); }
	int frameWidth() const { return m_frame->width; }
	int frameHeight() const { return m_frame->height; }
	int64_t channelLayout() const { return m_frame->channel_layout; }
	AVSubtitle* sub() { return &m_sub; }
	int uploaded() const { return m_uploaded; }
	void setWidth(int width) { m_width = width; }
	int width() const { return m_width; }
	void setHeight(int height) { m_height = height; }
	int height() const { return m_height; }
	const AVRational &sar() const { return m_sar; }
	int frameLineSize0() const { return m_frame->linesize[0]; }

	void setPosInfo(double pts, int64_t pos, int serial, double duration) {
		m_pts = pts;
		m_pos = pos;
		m_serial = serial;
		m_duration = duration;
	}

	void setSAR(const AVRational &sar) {
		m_sar = sar;
	}

	void setPictureInfo(int width, int height, int format) {
		m_width = width;
		m_height = height;
		m_format = format;
	}

	void setUploaded(int uploaded) {
		m_uploaded = uploaded;
	}

	void moveRef(AVFrame *frame);
	double pts() const { return m_pts; }
	double duration() const { return m_duration; }
	AVFrame *frame() const { return m_frame; }
	void setFlipV(int flipV) { m_flipV = flipV; }
	int flipV() const { return m_flipV; }
	uint16_t subFormat() const {
		return m_sub.format;
	}
	int64_t subPts() const {
		return m_sub.pts;
	}
	void setSerial(int serial) {
		m_serial = serial;
	}
	void setAreaInfo(int width, int height) {
		m_width = width;
		m_height = height;
	}
	void setPts(double pts) {
		m_pts = pts;
	}

private:
	AVFrame *m_frame = nullptr;
	AVSubtitle m_sub = { 0, };
	int m_serial = 0;
	double m_pts = 0;
	double m_duration = 0;
	int64_t m_pos = 0;
	int m_width = 0;
	int m_height = 0;
	int m_format = 0;
	AVRational m_sar = { 0, 0 };
	int m_uploaded = 0;
	int m_flipV = 0;
};

class FrameQueue
{
public:
	FrameQueue(PacketQueue &pktQ, int maxSize, int keepLast);
	~FrameQueue();

public:
	void signal();
	Frame *peek();
	Frame *peekNext();
	Frame *peekLast();
	Frame *peekWritable();
	Frame *peekReadable();
	void push();
	void next();
	int remaining() const;
	int64_t lastPos() const;
	int rIndexShown() const;
	
	void lock();
	void unlock();

public:
	enum {
		VIDEO_PICTURE_QUEUE_SIZE = 3,
		SUBPICTURE_QUEUE_SIZE = 16,
		SAMPLE_QUEUE_SIZE = 9,
		FRAME_QUEUE_SIZE = FFMAX(SAMPLE_QUEUE_SIZE, FFMAX(VIDEO_PICTURE_QUEUE_SIZE, SUBPICTURE_QUEUE_SIZE))
	};

private:
	Frame m_queue[FRAME_QUEUE_SIZE];
	int m_rIndex = 0;
	int m_wIndex = 0;
	int m_size = 0;
	int m_maxSize = 0;
	int m_keepLast = 0;
	int m_rIndexShown = 0;
	std::unique_ptr<Mutex> m_mutex;
	std::unique_ptr<Condition> m_cond;
	PacketQueue &m_pktQ;
};

