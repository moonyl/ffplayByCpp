#pragma once
#ifndef _CLOCK_H_
#define _CLOCK_H_

class PacketQueue;
class Clock
{
public:
	Clock(PacketQueue &pQ);
	~Clock();

public:
	double getClock() const;
	void setClockAt(double pts, int serial, double time);
	void setClock(double pts, int serial);
	void setClockSpeed(double speed);
	void syncClock(const Clock &slave);
	int serial() const { return m_serial; }
	double lastUpdated() { return m_lastUpdated; }
	void setPaused(int paused) { m_paused = paused; }
	double speed() const { return m_speed; }
	double pts() const { return m_pts; }

private:
	double m_pts;
	double m_ptsDrift;
	double m_lastUpdated;
	double m_speed;
	int m_serial;
	int m_paused;	
	PacketQueue &m_packetQ;
	const double AV_NOSYNC_THRESHOLD = 10.0;
};

#endif