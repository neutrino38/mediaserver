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
#include <mutex>
// En-têtes mcu d'abord : ils fixent les gardes d'inclusion (media/audio/video/
// codecs) de sorte que <medkit/ffmediafilewriter.h>, inclus par le .cpp,
// réutilise ces mêmes définitions (partagées avec libmedikit).
#include "medkit/codecs.h"
#include "audio.h"
#include "video.h"
#include "text.h"
#include "media.h"
#include "recordercontrol.h"
#include "Joinable.h"
#include "redcodec.h"
#include <map>
#include <vector>

// Moteur d'écriture de fichier média de libmedkit (libavformat). Déclaration en
// avant pour ne pas imposer <medkit/ffmediafilewriter.h> aux consommateurs de
// cet en-tête, comme mp4recorder.h le fait pour mp4writer.
class FfMediaFileWriter;

/**
 * Enregistreur d'une jambe JSR-309 : dépaquétise les flux RTP qui lui sont
 * attachés et les écrit dans un fichier média, par le FfMediaFileWriter de
 * libmedkit. Le CONTENEUR suit l'extension du nom de fichier (.mp4, .mkv...),
 * et avec lui les codecs enregistrables sans transcodage.
 *
 * Contrainte de libavformat, qui gouverne toute cette classe : aucune piste ne
 * peut naître après la première trame écrite. Or une source RTP ne livre son
 * codec qu'avec son premier paquet. D'où le partage : la piste texte se déclare
 * dès Create() (son codec ne dépend de rien), l'audio et la vidéo sont
 * ANNONCÉES à Create() par FfMediaFileWriter::ExpectTrack puis déclarées à leur
 * première trame. Conséquence pour l'appelant : les Attach doivent précéder
 * RecorderRecord, un média attaché ensuite n'a plus de piste où aller.
 */
class Recorder :
	public RecorderControl,
	public MediaFrame::Listener,
	public Joinable::Listener
{
public:
	Recorder(std::wstring tag);
	virtual ~Recorder();

	//RecorderControl
	virtual bool Create(const char *filename);
	virtual bool Record();
	virtual bool Stop();
	virtual bool Close();
	virtual RecorderControl::Type GetType()	{ return RecorderControl::MP4;	}

	//MediaFrame::Listener
	virtual void onMediaFrame(MediaFrame &frame);

	//À appeler avant Create() : si false, l'audio et le texte s'enregistrent
	//sans attendre la première I-frame vidéo (appels sans vidéo notamment).
	void SetWaitVideo(bool wait)	{ waitVideo = wait; }

	//Joinable::Listener
	virtual void onRTPPacket(RTPPacket &packet);
	virtual void onResetStream();
	virtual void onEndStream();

	//Attach
	int Attach(MediaFrame::Type media, const std::shared_ptr<Joinable> & join);
	int Dettach(MediaFrame::Type media);

	//Une source est attachée pour ce média ET négociée côté source
	//(Joinable::IsReceiving — les Endpoint::Port reflètent StartReceiving)
	bool MediaIsActive(MediaFrame::Type media);

	//Écho vidéo : renvoyer chaque paquet vidéo reçu vers l'émetteur de la
	//source (RecorderRecord arg 6, éteint par RecorderStop)
	void SetEchoVideo(bool echo)	{ echoVideo = echo; }

	std::wstring& GetTag() { return tag; }
private:
	//Liens retour NON possédants vers les sources : weak_ptr → lock() au site
	//d'usage. Une source détruite avant nous fait échouer le lock() (le Dettach/
	//~Recorder ultérieur ne déréférence pas d'objet libéré) — C-13, lien A.
	typedef std::map<MediaFrame::Type,std::weak_ptr<Joinable>> JoinedMap;

	//Reçoit les TextFrame issues du décodage RED et les transmet au fichier, en
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

	//Un fichier est ouvert ET l'enregistrement est en cours
	bool IsRecording();
	//Le conteneur du fichier porte-t-il ce codec audio tel quel ? Sinon, la
	//voie audio passe par un transcodage AAC.
	bool AudioFitsFile(AudioCodec::Type codec);
	//Déclare la piste audio du fichier, à sa première trame. @return false si
	//le fichier n'en veut pas : rien à écrire pour ce média.
	bool EnsureAudioTrack(AudioCodec::Type codec,DWORD rate);
private:
	std::wstring tag;
	RTPDepacketizer* video;
	RTPDepacketizer* audio;
	//Transcodage vers AAC des codecs audio que le conteneur n'accepte pas.
	//L'encodeur accumule lui-même jusqu'à sa trame complète (1024 échantillons
	//pour l'AAC) : rien à réassembler ici.
	AudioDecoder*	audioDecoder;
	AudioEncoder*	audioEncoder;
	DWORD		audioRate;
	QWORD		audioSamples;
	//Moteur d'écriture : il possède le fichier, sa destruction écrit le trailer
	std::unique_ptr<FfMediaFileWriter> writer;
	std::mutex	mutex;		// sérialise onMediaFrame avec Create/Close
	bool		recording = false;
	bool		waitVideo = true;	// attendre la 1re I-frame avant d'écrire
	//Pistes dont le sort est joué : déclarées, ou refusées par le conteneur —
	//dans les deux cas il ne faut plus rappeler AddTrack (qui journalise).
	bool		audioTrackDone = false;
	bool		audioTrackOk = false;
	bool		videoTrackDone = false;
	//Décodage de la redondance T.140 (RFC 4103)
	RedundentCodec	redCodec;
	TextForwarder	textForwarder;
	timeval		recStart;	// instant du Create(), origine de l'axe temps
	JoinedMap	 joined;
	//Copie dédiée de joined[Video] pour l'écho : lue par le thread RTP vidéo
	//sans traverser la map (que les Attach/Dettach XML-RPC peuvent muter) —
	//même motif weak_ptr + lock() au site d'usage que VideoStream::rtpSession
	std::weak_ptr<Joinable> videoSource;
	bool		echoVideo = false;
};

#endif	/* RECORDER_H */

