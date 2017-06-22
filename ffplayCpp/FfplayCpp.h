#ifndef _FFPLAY_CPP_H_
#define _FFPLAY_CPP_H_

class VideoState;
class FfPlayCpp
{
public:
	FfPlayCpp();
	~FfPlayCpp();

public:
	void openStream(const char* inputFile, AVInputFormat *inputFormat);
	void eventLoop();

private:
	void init();
	void cleanup();	

	void initSDL();
	void initAv();
	void doExit(void *is/*VideoState *is*/);

	static int lockmgr(void **mtx, enum AVLockOp op);

private:
	static void handleSigTerm(int sig);

private:
	VideoState *m_videoState = nullptr;
};

#endif


