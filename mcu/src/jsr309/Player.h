/* 
 * File:   Player.h
 * Author: Sergio
 *
 * Created on 9 de septiembre de 2011, 0:11
 */

#ifndef PLAYER_H
#define	PLAYER_H

#include "RTPMultiplexer.h"
#include "mp4streamer.h"


class Player :
	public MP4Streamer,
	public MP4Streamer::Listener
{
public:
	class Listener
	{
	public:
		//Virtual desctructor
		virtual ~Listener(){};
	public:
		//Interface
		virtual void onEndOfFile(Player *player,void* param) = 0;
	};
public:
	Player(std::wstring tag);
	virtual ~Player() {}
	void SetListener(Player::Listener *listener,void* param);
	Joinable* GetJoinable(MediaFrame::Type media);

	// Choisit, parmi les codecs présents dans le fichier, une alternative
	// acceptée par TOUS les endpoints attachés (via RTPMultiplexer::TryCodec),
	// puis re-sélectionne la piste correspondante dans le MP4Streamer. À appeler
	// après l'attach et avant Play. Sans effet si aucun endpoint n'est attaché
	// ou si aucun codec commun n'est trouvé (la sélection par défaut d'Open
	// reste alors en place).
	void NegotiateCodecs();

	/* MP4Streamer listener*/
	virtual void onRTPPacket(RTPPacket &packet);
	virtual void onTextFrame(TextFrame &text);
	virtual void onMediaFrame(MediaFrame &frame);
	virtual void onEnd();

	std::wstring& GetTag() { return tag;	}
	
	int SetEventContextId( MediaFrame::Type media, int ctxId );
	int SetEventHandler( MediaFrame::Type media, int sessionId,	JSR309Manager* jsrManager);
private:
	RTPMultiplexer audio;
	RTPMultiplexer video;
	RTPMultiplexer text;
	std::wstring tag;
	Player::Listener *listener;
	void* param;
};

#endif	/* PLAYER_H */

