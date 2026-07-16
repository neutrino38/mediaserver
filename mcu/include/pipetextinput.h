#ifndef _PIPETEXTINPUT_H_
#define _PIPETEXTINPUT_H_
#include <condition_variable>
#include <mutex>
#include <atomic>
#include "text.h"
#include <list>

class PipeTextInput :
	public TextInput
{
public:
	PipeTextInput();
	virtual ~PipeTextInput();
	virtual TextFrame* GetFrame(DWORD timeout);
	virtual void Cancel();
	int Init();
	int WriteText(const wchar_t *data,DWORD size);
	int WriteText(const std::wstring &str);
	int End();

private:
	//Los mutex y condiciones
	std::mutex mutex;
	std::condition_variable cond;

	//Members
	std::list<TextFrame*> frames;
	std::atomic<bool> inited;
	timeval		first;
};

#endif
