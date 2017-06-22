#include "Clock.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/time.h>
}


Clock::Clock(const int &queueSerial) : 
	m_queueSerial(queueSerial),
	m_speed(1.0),
	m_serial(0)
{
	setClock(NAN, -1);
}


Clock::~Clock()
{
}

double Clock::getClock() const
{
	if (m_queueSerial != m_serial) {
		return NAN;
	}

	if (m_paused) {
		return m_pts;
	}
	
	double time = av_gettime_relative() / 1000000.0;
	return m_ptsDrift + time - (time - m_lastUpdated) * (1.0 - m_speed);
}

void Clock::setClockAt(double pts, int serial, double time)
{
	m_pts = pts;
	m_lastUpdated = time;
	m_ptsDrift = m_pts - time;
	m_serial = serial;
}

void Clock::setClock(double pts, int serial)
{
	double time = av_gettime_relative() / 1000000.0;
	setClockAt(pts, serial, time);
}

void Clock::setClockSpeed(double speed)
{
	setClock(getClock(), m_serial);
	m_speed = speed;
}

void Clock::syncClock(const Clock & slave)
{
	double clock = getClock();
	double slaveClock = slave.getClock();
	if (!isnan(slaveClock) && (isnan(clock) || fabs(clock - slaveClock) > AV_NOSYNC_THRESHOLD)) {
		setClock(slaveClock, slave.m_serial);
	}
}
