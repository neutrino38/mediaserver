#ifndef TEXTENCODER_H_
#define	TEXTENCODER_H_
#include <atomic>
#include <mutex>
#include "text.h"
#include "worker.h"
#include <set>

class TextEncoder : public Worker
{
public:
	TextEncoder();
	~TextEncoder();

	int Init(TextInput *input);
	bool AddListener(MediaFrame::Listener *listener);
	bool RemoveListener(MediaFrame::Listener *listener);
	int StartEncoding();
	int StopEncoding();
	int End();

	int IsEncoding() { return encodingText;}

protected:
	int Encode();
	//Corps du Worker
	virtual int Run() { return Encode(); }

private:
	typedef std::set<MediaFrame::Listener*> Listeners;
	
private:
	Listeners		listeners;
	TextInput*		textInput;
	std::mutex		mutex;
	//Atomique : le thread d'encodage le lit en condition de boucle, le plan de
	//contrôle l'écrit.
	std::atomic<int>	encodingText;

	//Réveil de la boucle quand `GetFrame` rend NULL sans avoir attendu (pipe
	//non inité) : sans lui, la boucle tourne à vide sur un cœur.
	static const DWORD	IdleWaitMs = 1000;
};

#endif	/* TEXTENCODER_H */

