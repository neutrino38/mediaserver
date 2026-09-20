#include <list>
#include "log.h"
#include "pipetextinput.h"
#include "medkit/codecs.h"

PipeTextInput::PipeTextInput() : inited(false)
{
}

PipeTextInput::~PipeTextInput()
{
	//Clean any frame in the que
	while(frames.size())
	{
		//delete delete
		delete(frames.front());
		//Deque
		frames.pop_front();
	}
}

void PipeTextInput::Cancel()
{
	//SOUS LE VERROU, comme WriteText et comme les autres pipes. Sans lui, le
	//réveil est PERDU : le lecteur tient le verrou, a déjà lu `inited` à true
	//et n'est pas encore entré dans cond.wait_for ; notify_all ne trouve alors
	//aucun waiter, et GetFrame dort son timeout entier — jusqu'à 25 s pour le
	//flux texte, 10 s pour le pont WebSocket, autant de join qui n'en revient
	//pas.
	std::lock_guard<std::mutex> lock(mutex);

	//No estamos iniciados
	inited.store(false);

	//Terminamos
	cond.notify_all();
}

TextFrame* PipeTextInput::GetFrame(DWORD timeout)
{
	TextFrame *frame = NULL;
	timespec ts;
	
	//Bloqueamos
	std::unique_lock<std::mutex> lock(mutex);

	//Id we do not have enougth text  samples
	if (inited.load() && !frames.size())
	{
		//Check timeout
		if (timeout)
		{
			//Calculate timeout
			calcTimout(&ts,timeout);
			//Esperamos la condicion
			cond.wait_for(lock, std::chrono::milliseconds(timeout));
		} else {
			//Esperamos la condicion
			cond.wait(lock);
		}
	}

	if (frames.size())
	{
		//Get fist
		frame = frames.front();
		//Dequeue
		frames.pop_front();
	}
	
	return frame;
}

int PipeTextInput::WriteText(const std::wstring &str)
{
	//WriteText
	return WriteText(str.c_str(),str.length());
}

int PipeTextInput::WriteText(const wchar_t *data,DWORD size)
{
	//Bloqueamos
	std::lock_guard<std::mutex> lock(mutex);

	//Si estamos reproduciendo
	if (inited)
	{
		//Pop new frame
		frames.push_back(new TextFrame(getDifTime(&first)/1000,data,size));

		//Signal write
		cond.notify_all();
	} else
		Log("Not inited\n");

	//Salimos
	return true;
}

int PipeTextInput::Init()
{
	//Protegemos
	std::lock_guard<std::mutex> lock(mutex);

	//Iniciamos
	inited.store(true);

	//Set first timestamp
	getUpdDifTime(&first);

	return true;
}

int PipeTextInput::End()
{
	Log(">PipeTextInput End\n");

	//Même raison que Cancel() : le réveil se signale sous le verrou.
	{
		std::lock_guard<std::mutex> lock(mutex);

		//No estamos iniciados
		inited.store(false);

		//Terminamos
		cond.notify_all();
	}

	Log("<PipeTextInput Ended\n");

	//Salimos
	return true;
}
