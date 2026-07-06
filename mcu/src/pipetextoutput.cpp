#include "log.h"
#include "pipetextoutput.h"
#include "text.h"

PipeTextOutput::PipeTextOutput()
{
}

PipeTextOutput::~PipeTextOutput()
{
}

int PipeTextOutput::SendFrame(TextFrame& frame)
{
	//Bloqueamos
	std::lock_guard<std::mutex> lock(mutex);

	//Get string
	DWORD len = frame.GetWLength();

	//Si no cabe
	if(fifoBuffer.length()+len>1024)
		//Limpiamos
		fifoBuffer.clear();

	//Metemos en la fifo
	fifoBuffer.push(frame.GetWChar(),len);

	return len;
}

int PipeTextOutput::PeekText(wchar_t *buffer,DWORD size)
{
	//Bloqueamos
	std::lock_guard<std::mutex> lock(mutex);

	//Obtenemos la longitud
	int len = fifoBuffer.length();

	//Miramos si hay suficientes
	if (len > size)
		//Set maximun
		len = size;

	//OBtenemos las muestras
	fifoBuffer.peek(buffer,len);

	//Salimos
	return len;
}

int PipeTextOutput::SkipText(DWORD size)
{
	//Bloqueamos
	std::lock_guard<std::mutex> lock(mutex);

	//Obtenemos la longitud
	int len = fifoBuffer.length();

	//Miramos si hay suficientes
	if (len > size)
		//Set maximun
		len = size;

	//OBtenemos las muestras
	fifoBuffer.remove(len);

	//Salimos
	return len;
}

int PipeTextOutput::ReadText(wchar_t *buffer,DWORD size)
{
	//Bloqueamos
	std::lock_guard<std::mutex> lock(mutex);

	//Obtenemos la longitud
	int len = fifoBuffer.length();

	//Miramos si hay suficientes
	if (len > size)
		//Set maximun
		len = size;

	//OBtenemos las muestras
	fifoBuffer.pop(buffer,len);

	//Salimos
	return len;
}

int PipeTextOutput::Length()
{
	//Bloqueamos
	std::lock_guard<std::mutex> lock(mutex);

	//Obtenemos la longitud
	int len = fifoBuffer.length();

	//Salimos
	return len;
}

int PipeTextOutput::Init()
{
	Log("PipeTextOutput init\n");

	//Protegemos
	std::lock_guard<std::mutex> lock(mutex);

	//Iniciamos
	inited = true;

	return true;
} 

int PipeTextOutput::End()
{
	//Protegemos
	std::lock_guard<std::mutex> lock(mutex);

	//Terminamos
	inited = false;

	return true;
} 
