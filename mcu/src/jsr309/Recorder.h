/* 
 * File:   Recorder.h
 * Author: Sergio
 *
 * Created on 26 de febrero de 2012, 16:50
 */

#ifndef RECORDER_H
#define	RECORDER_H

#include "config.h"
#include <string>
#include <memory>
#include "Joinable.h"
#include "mp4recorder.h"
#include "redcodec.h"
#include <map>
#include <vector>

class Recorder :
	public MP4Recorder,
	public Joinable::Listener
{
public:
	Recorder(std::wstring tag);
	virtual ~Recorder();

	//RecorderControl (surcharge : mémorise l'instant de départ pour situer
	//les médias intermittents — le texte — sur l'axe de l'enregistrement)
	virtual bool Create(const char *filename);

	//Joinable::Listener
	virtual void onRTPPacket(RTPPacket &packet);
	virtual void onResetStream();
	virtual void onEndStream();

	//Attach
	int Attach(MediaFrame::Type media, const std::shared_ptr<Joinable> & join);
	int Dettach(MediaFrame::Type media);

	std::wstring& GetTag() { return tag; }
private:
	//Liens retour NON possédants vers les sources : weak_ptr → lock() au site
	//d'usage. Une source détruite avant nous fait échouer le lock() (le Dettach/
	//~Recorder ultérieur ne déréférence pas d'objet libéré) — C-13, lien A.
	typedef std::map<MediaFrame::Type,std::weak_ptr<Joinable>> JoinedMap;

	//Reçoit les TextFrame issues du décodage RED et les transmet au MP4, en
	//filtrant les keepalives T.140 (BOM UTF-8, trames vides) et en rebasant
	//les timestamps RTP (origine aléatoire) sur l'axe de l'enregistrement :
	//la première trame est ancrée à l'horloge murale écoulée depuis Create(),
	//les suivantes gardent leurs deltas RTP (horloge T.140 = 1 kHz = ms).
	class TextForwarder : public TextOutput
	{
	public:
		TextForwarder(Recorder &rec) : rec(rec) {}
		virtual int SendFrame(TextFrame &frame);
		void Reset()	{ baseSet = false; }
	private:
		Recorder &rec;
		DWORD	base = 0;
		DWORD	offset = 0;
		bool	baseSet = false;
	};

	void onAudioPacket(RTPPacket &packet);
	void onTextPacket(RTPPacket &packet);
private:
	std::wstring tag;
	RTPDepacketizer* video;
	RTPDepacketizer* audio;
	//Transcodage vers AAC des codecs audio que le conteneur MP4 n'accepte pas.
	//Le PCM décodé est accumulé dans pcmFifo : l'encodeur ffmpeg exige des
	//appels avec une trame complète (1024 échantillons) en entrée.
	AudioDecoder*	audioDecoder;
	AudioEncoder*	audioEncoder;
	DWORD		audioRate;
	QWORD		audioSamples;
	std::vector<SWORD> pcmFifo;
	//Décodage de la redondance T.140 (RFC 4103)
	RedundentCodec	redCodec;
	TextForwarder	textForwarder;
	timeval		recStart;	// instant du Create(), origine de l'axe temps
	JoinedMap	 joined;
};

#endif	/* RECORDER_H */

