#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <signal.h>
#include "textstream.h"
#include "log.h"
#include "tools.h"

static BYTE BOMUTF8[]			= {0xEF,0xBB,0xBF};
static BYTE LOSTREPLACEMENT[]		= {0xEF,0xBF,0xBD};

/**********************************
* TextStream
*	Constructor
***********************************/
TextStream::TextStream(RTPSession::Listener* listener) :
	rtp(MediaFrame::Text,listener),
	//Le porteur du pont est CETTE session : elle est déjà ICE + DTLS + UDP.
	bridge(rtp,*this)
{
	sendingText=TaskIdle;
	receivingText=TaskIdle;
	textCodec=TextCodec::T140;
	muted = false;
	transport = MediaFrame::RTP;
	gettimeofday(&clock,NULL);
}

/*******************************
* ~TextStream
*	Destructor. 
********************************/
TextStream::~TextStream()
{
	//Défense en profondeur (H-5) : arrêt/join des threads même sans End()
	//préalable. End() est idempotent.
	End();
}

/***************************************
* SetTextCodec
*	Fija el codec de text
***************************************/
int TextStream::SetTextCodec(TextCodec::Type codec)
{
	//Compromabos que soportamos el modo
	if (!(codec==TextCodec::T140 || codec==TextCodec::T140RED))
		return 0;

	//Colocamos el tipo de text
	textCodec = codec;

	Log("-SetTextCodec [%d,%s]\n",textCodec,TextCodec::GetNameFor(textCodec));

	//Y salimos
	return 1;	
}

/***************************************
* SetTransport
*	RTP ou data channel. La session RTP reste le porteur dans les deux cas ;
*	seul change ce qui voyage dedans.
***************************************/
int TextStream::SetTransport(MediaFrame::MediaProtocol proto)
{
	if (proto != MediaFrame::RTP && proto != MediaFrame::SCTP)
		return Error("-TextStream::SetTransport() | transport %s non supporte pour du texte\n",
				MediaFrame::ProtocolToString(proto));

	if (proto == transport)
		return 1;

	//Changer de dialecte sous les pieds des threads en cours n'aurait pas de sens.
	if (sendingText != TaskIdle || receivingText != TaskIdle)
		return Error("-TextStream::SetTransport() | a poser avant StartReceiving/StartSending\n");

	transport = proto;

	//En mode data channel, le pont se branche tout de suite : l'association
	//s'ouvrira dès que le handshake DTLS sera terminé, sans attendre que le
	//controleur ouvre le plan de reception.
	if (transport == MediaFrame::SCTP)
		bridge.Start();

	Log("-SetTransport text [%s]\n",MediaFrame::ProtocolToString(transport));
	return 1;
}

/***************************************
* SetupDataChannel
*	Ce que le serveur sait de lui-meme, c'est a lui qu'on le demande : le
*	`a=sctp-port` et le `a=max-message-size` que le controleur publie sortent
*	d'ici, pas d'une constante recopiee de son cote.
***************************************/
int TextStream::SetupDataChannel(WORD remoteSCTPPort,WORD& localSCTPPort,
				 DWORD& maxMessageSize,int& streamId)
{
	if (transport != MediaFrame::SCTP)
		return Error("-TextStream::SetupDataChannel() | le flux texte n'est pas un data channel,"
			     " poser le transport SCTP d'abord\n");

	bridge.SetRemoteSCTPPort(remoteSCTPPort);

	localSCTPPort  = bridge.GetLocalSCTPPort();
	maxMessageSize = bridge.GetMaxMessageSize();
	streamId       = bridge.GetStreamId();

	return 1;
}

/***************************************
* onT140Block
*	Un T140block recu du pair, a remettre au mixeur texte. On est sur le thread
*	de la session, pas sur celui d'un thread de reception : il n'y en a pas en
*	mode data channel.
***************************************/
void TextStream::onT140Block(const BYTE* data,DWORD size)
{
	if (!size || !textOutput)
		return;

	//Le constructeur analyse l'UTF-8 dans la chaine large de la trame — c'est
	//elle que PipeTextOutput::SendFrame consomme (GetWChar/GetWLength). Des
	//octets posés sans analyse nourriraient le mixeur d'une chaine vide.
	TextFrame frame(getDifTime(&clock)/1000,data,size);

	if (frame.GetWLength())
		textOutput->SendFrame(frame);
}

void TextStream::onT140ChannelOpen()
{
	Log("-TextStream: canal t140 ouvert [%p]\n",this);
}

void TextStream::onT140ChannelLost()
{
	//T.140 §5.3 : le cote qui SURVIT — la conference — apprend la perte par un
	//U+FFFD dans le flux. C'est la seule trace qu'un utilisateur ait qu'il
	//manque du texte.
	Log("-TextStream: canal t140 perdu, U+FFFD vers le mixeur [%p]\n",this);
	onT140Block(LOSTREPLACEMENT,sizeof(LOSTREPLACEMENT));
}

/***************************************
* Init
*	Inicializa los devices 
***************************************/
int TextStream::Init(std::shared_ptr<TextInput> input, std::shared_ptr<TextOutput> output)
{
	Log(">Init text stream (shared)\n");

	//Iniciamos el rtp
	if(!rtp.Init())
		return Error("No hemos podido abrir el rtp\n");

	//Co-propriété du pipe du mixer (Point 1 / C-4).
	textInput  = std::move(input);
	textOutput = std::move(output);

	//Y aun no estamos mandando nada
	sendingText=TaskIdle;
	receivingText=TaskIdle;

	Log("<Init text stream (shared)\n");

	return 1;
}

int TextStream::SetLocalCryptoSDES(const char* suite, const char* key64)
{
	return rtp.SetLocalCryptoSDES(suite,key64);
}

int TextStream::SetRemoteCryptoSDES(const char* suite, const char* key64)
{
	return rtp.SetRemoteCryptoSDES(suite,key64);
}

int TextStream::SetRemoteCryptoDTLS(const char *setup,const char *hash,const char *fingerprint)
{
	return rtp.SetRemoteCryptoDTLS(setup, hash, fingerprint);
}


int TextStream::SetLocalSTUNCredentials(const char* username, const char* pwd)
{
	return rtp.SetLocalSTUNCredentials(username,pwd);
}

int TextStream::SetRemoteSTUNCredentials(const char* username, const char* pwd)
{
	return rtp.SetRemoteSTUNCredentials(username,pwd);
}
int TextStream::SetRTPProperties(const Properties& properties)
{
	return rtp.SetProperties(properties);
}
/***************************************
* startSendingText
*	Helper function
***************************************/
void * TextStream::startSendingText(void *par)
{
	TextStream *conf = (TextStream *)par;
	blocksignals();
	Log("SendTextThread [%d]\n",getpid());
	pthread_exit((void *)(intptr_t)conf->SendText());
}

/***************************************
* startReceivingText
*	Helper function
***************************************/
void * TextStream::startReceivingText(void *par)
{
	TextStream *conf = (TextStream *)par;
	blocksignals();
	Log("RecvTextThread [%d]\n",getpid());
	pthread_exit((void *)(intptr_t)conf->RecText());
}

/***************************************
* StartSending
*	Comienza a mandar a la ip y puertos especificados
***************************************/
int TextStream::StartSending(char *sendTextIp,int sendTextPort,RTPMap& rtpMap)
{
	Log(">StartSending text [%s,%d]\n",sendTextIp,sendTextPort);

	//Si estabamos mandando tenemos que parar
	StopSending();
	
	if (sendingText != TaskIdle)
		return Error("Cannot start sending text: bad state.\n");
	
	//Si tenemos text
	if (sendTextPort==0)
		//Error
		return Error("Text port 0\n");


	//Y la de text
	if(!rtp.SetRemotePort(sendTextIp,sendTextPort))
		//Error
		return Error("Error en el SetRemotePort\n");

	//Data channel : ni rtpMap d'emission, ni codec — rien de tout cela ne voyage
	//dedans. La destination vient d'etre posee, ce qui suffit a ICE et au DTLS.
	if (transport == MediaFrame::SCTP)
	{
		bridge.Start();

		sendingText = TaskStarting;
		createPriorityThread(&sendTextThread,startSendingText,this,1);

		Log("<StartSending text sur data channel [%d]\n",sendingText);
		return 1;
	}

	//Set sending map
	rtp.SetSendingRTPMap(rtpMap);

	//Get t140 for redundancy
	for (RTPMap::iterator it = rtpMap.begin(); it!=rtpMap.end(); ++it)
	{
		//Is it ourr codec
		if (it->second==TextCodec::T140)
		{
			//Set it
			t140Codec = it->first;
			//and we are done
			continue;
		}
	}

	//Set text codec
	if(!rtp.SetSendingCodec(textCodec))
		//Error
		return Error("%s text codec not supported by peer\n",TextCodec::GetNameFor(textCodec));

	//Estamos mandando
	sendingText = TaskStarting;

	//Start thread
	createPriorityThread(&sendTextThread,startSendingText,this,1);

	Log("<StartSending text [%d]\n",sendingText);

	return 1;
}

/***************************************
* StartReceiving
*	Abre los sockets y empieza la recetpcion
****************************************/
int TextStream::StartReceiving(RTPMap& rtpMap)
{
	//If already receiving
	StopReceiving();
	
	if (receivingText != TaskIdle)
		return Error("Failed to start receiving text. Task in bad state.\n");

	
	//Get local rtp port
	int recTextPort = rtp.GetLocalPort();

	//Data channel : aucun payload type ne voyage dedans, la rtpMap ne dit rien de
	//CE transport. Et pas de thread de reception : les blocs entrants arrivent sur
	//le thread de la session et vont droit au pipe du mixeur.
	if (transport == MediaFrame::SCTP)
	{
		bridge.Start();

		receivingText = TaskRunning;

		Log("<StartReceiving text sur data channel [%d]\n",recTextPort);
		return recTextPort;
	}

	//Set receving map
	rtp.SetReceivingRTPMap(rtpMap);

	//P7/S2 : nouveau cycle de reception => on rearme la notification « premier
	//paquet RTP/SRTP recu », pour que « media etabli » soit signale une fois PAR
	//cycle (un StopReceiving/StartReceiving le redeclenche) et pas une seule fois
	//dans la vie du participant.
	rtp.ArmRTPReceivedNotification();

	//Estamos recibiendo
	receivingText= TaskStarting;

	//Create thread
	createPriorityThread(&recTextThread,startReceivingText,this,1);

	//Log
	Log("<StartReceiving text [%d]\n",recTextPort);

	//Return receiving port
	return recTextPort;
}

/***************************************
* End
*	Termina la conferencia activa
***************************************/
int TextStream::End()
{
	//Terminamos de enviar
	StopSending();

	//Y de recivir
	StopReceiving();

	//Cerramos la session de rtp
	rtp.End();

	//ORDRE : la boucle de la session bat la cadence de la pile SCTP et vide sa
	//file de sortie. On ne demonte le pont qu'une fois cette boucle arretee —
	//l'inverse est une course. No-op si le flux etait en RTP.
	bridge.Stop();

	return 1;
}

/***************************************
* StopReceiving
* 	Termina la recepcion
****************************************/

int TextStream::StopReceiving()
{
	Log(">StopReceiving Text\n");

	//Data channel : il n'y a pas de thread de reception a joindre. Le pont, lui,
	//reste branche — une re-negociation ne doit pas fermer un canal que le
	//navigateur tient toujours ouvert. C'est End() qui le demonte.
	if (transport == MediaFrame::SCTP)
	{
		receivingText = TaskIdle;
		Log("<StopReceiving Text (data channel)\n");
		return 1;
	}

	//Y esperamos a que se cierren las threads de recepcion
	if (receivingText  == TaskRunning || receivingText  ==TaskStarting)
	{	
		//Dejamos de recivir
		receivingText = TaskStopping;

		//Cancel rtp
		rtp.CancelGetPacket();
		
		//Y unimos
		pthread_join(recTextThread,NULL);
	}

	Log("<StopReceiving Text\n");

	return 1;

}

/***************************************
* StopSending
* 	Termina el envio
****************************************/
int TextStream::StopSending()
{
	Log(">StopSending Text\n");

	//Esperamos a que se cierren las threads de envio
	if (sendingText == TaskRunning || sendingText == TaskStarting)
	{
		//Paramos el envio
		sendingText = TaskStopping;

		//Cancel grab if any
		textInput->Cancel();

		//Y esperamos
		pthread_join(sendTextThread,NULL);
	}

	Log("<StopSending Text\n");

	return 1;	
}


/****************************************
* RecText
*	Obtiene los packetes y los muestra
*****************************************/
int TextStream::RecText()
{
	BYTE*		text;
	DWORD		textSize;
	DWORD		lastSeq = RTPPacket::MaxExtSeqNum;

	Log(">RecText\n");
	rtp.ResetPacket(false);
	//Ne PAS ecraser un TaskStopping que StopReceiving/StopSending vient de poser
	//pendant que ce thread demarrait : le drapeau repartait a TaskRunning, la
	//boucle ne sortait plus JAMAIS, et le pthread_join de l'appelant restait
	//bloque a vie — avec lui le thread XML-RPC, puis End() et l'arret du
	//processus (SIGTERM sans effet). Garde deja en place dans videostream.cpp.
	if (receivingText == TaskStarting) receivingText = TaskRunning;
	//Mientras tengamos que capturar
	while(receivingText  == TaskRunning)
	{
		//Get packet
		RTPPacket* packet = rtp.GetPacket(0,RTPSession::ConsumerPollMs);

		//Check if gor anti
		if (!packet)
                {
			//GetPacket a deja attendu : relire le drapeau et repartir.
			continue;
                }

		//Get type
		TextCodec::Type type = (TextCodec::Type)packet->GetCodec();

		//Get timestamp
		DWORD timeStamp = packet->GetTimestamp();

		//Get extended sequence number
		DWORD seq = packet->GetExtSeqNum();

		//Lost packets since last one
		DWORD lost = 0;

		//If not first
		if (lastSeq!=RTPPacket::MaxExtSeqNum)
			//Calculate losts
			lost = seq-lastSeq-1;

		//Update last sequence number
		lastSeq = seq;

		//Check if we are muted
		if (!muted)
		{
			//Check the type of data
			if (type==TextCodec::T140RED)
			{
				//Get redundant packet
				RTPRedundantPacket* red = (RTPRedundantPacket*) packet;				
                                redCodec.Decode(red, textOutput.get());
			} 
                        else {
				//For each lost packet send a mark
				for (int i=0;i<lost;i++)
				{
					//Create frame of lost replacement
					TextFrame frame(timeStamp,LOSTREPLACEMENT,sizeof(LOSTREPLACEMENT));
					//Y lo reproducimos
					textOutput->SendFrame(frame);
				}
				//Create frame
				TextFrame frame(timeStamp,packet->GetMediaData(),packet->GetMediaLength());
				//Send it
				textOutput->SendFrame(frame);
			}
		}

		//Delete rtp packet
		delete(packet);
	}

	Log("<RecText\n");

	receivingText = TaskIdle;
	//Salimos
	pthread_exit(0);
}

/*******************************************
* SendText
*	Capturamos el text y lo mandamos
*******************************************/
/****************************************
* SendTextOverDataChannel
*	Le meme travail que SendText, sans RTP : on tire du pipe du mixeur et on
*	pousse un T140block. Aucun keepalive a emettre — ni BOM, ni bloc vide : SCTP
*	est fiable, et le texte est legitimement silencieux entre deux frappes.
*****************************************/
int TextStream::SendTextOverDataChannel()
{
	Log(">SendText sur data channel\n");

	//Ne PAS ecraser un TaskStopping que StopReceiving/StopSending vient de poser
	//pendant que ce thread demarrait : le drapeau repartait a TaskRunning, la
	//boucle ne sortait plus JAMAIS, et le pthread_join de l'appelant restait
	//bloque a vie — avec lui le thread XML-RPC, puis End() et l'arret du
	//processus (SIGTERM sans effet). Garde deja en place dans videostream.cpp.
	if (sendingText == TaskStarting) sendingText = TaskRunning;

	while (sendingText == TaskRunning)
	{
		TextFrame* frame = textInput->GetFrame(25000);

		//Expiration ou annulation : rien a dire.
		if (!frame)
			continue;

		if (!muted && frame->GetLength())
			bridge.SendText(frame->GetData(),frame->GetLength());

		delete frame;
	}

	sendingText = TaskIdle;

	Log("<SendText sur data channel\n");
	return 1;
}

int TextStream::SendText()
{
    //Data channel : autre dialecte, autre boucle.
    if (transport == MediaFrame::SCTP)
        return SendTextOverDataChannel();

    RTPPacket packet(MediaFrame::Text,textCodec);
    bool idle = true;
    DWORD timeout = 25000;
    DWORD lastTime = 0;

    Log(">SendText\n");
	//Ne PAS ecraser un TaskStopping que StopReceiving/StopSending vient de poser
	//pendant que ce thread demarrait : le drapeau repartait a TaskRunning, la
	//boucle ne sortait plus JAMAIS, et le pthread_join de l'appelant restait
	//bloque a vie — avec lui le thread XML-RPC, puis End() et l'arret du
	//processus (SIGTERM sans effet). Garde deja en place dans videostream.cpp.
    if (sendingText == TaskStarting) sendingText = TaskRunning;
    //Mientras tengamos que capturar
    while(sendingText == TaskRunning)
    {
        //Text frame
        TextFrame *frame = NULL;

        //Get frame
        frame = textInput->GetFrame(timeout);

        //Calculate last frame time
        if (frame)
            //Get it from frame
            lastTime = frame->GetTimeStamp();
        else
	{
	    msleep(200);
            //Update last send time with timeout
            lastTime += timeout;
	}

        //Check codec
        if (textCodec == TextCodec::T140) 
        {
            //Check frame
            if (frame) {
                //Set timestamp
                packet.SetTimestamp(lastTime);
                //Set data
                packet.SetPayload(frame->GetData(), frame->GetLength());

                //Set Mark for the first frame after idle
                packet.SetMark(idle);

                //Send it
                rtp.SendPacket(packet);

                //Delete frame
                delete(frame);

                //Not idle anymore
                idle = false;
            } 
            else {
                //Set data
                packet.SetPayload(BOMUTF8, sizeof (BOMUTF8));

                //No mark
                packet.SetMark(false);

                //Send it
                rtp.SendPacket(packet);
            }
        } 
        else {
            // Redundent text
            RTPRedundantPacket *redpak = NULL;
            
            if (frame) {
                redpak = redCodec.Encode(frame, t140Codec);
            }
            else 
            {
                if (idle) 
                {
                    redpak = redCodec.EncodeBOM(t140Codec);
                } 
                else 
                {
                    redpak = redCodec.EncodeNull(t140Codec);
                }
				if (redpak)
					redpak->SetTimestamp(lastTime);
            }
            //Send the mark bit if it is first frame after idle
            bool mark = idle && frame;
			
			if (redpak)
			{
				//Set mark
				redpak->SetMark(mark);

				//Send it
				rtp.SendPacket(*redpak);
			}
			
            //Calculate timeouts
            if ( redCodec.EncoderIsIdle() ) 
            {
                //By default wait for keep-alive
                timeout = 10000;
                idle = true;
            }
            else {
                
                //Normal timeout
                idle = false;
                timeout = 300;
            }

            delete redpak;
        }
    }


	//Salimos
	Log("<SendText\n");
	sendingText = TaskIdle;
	pthread_exit(0);
}

MediaStatistics TextStream::GetStatistics()
{
	MediaStatistics stats;

	//Fill stats
        rtp.GetStatistics(0, stats);
	stats.isReceiving	= IsReceiving();
	stats.isSending		= IsSending();

	//Return it
	return stats;
}

int TextStream::SetMute(bool isMuted)
{
	//Set it
	muted = isMuted;
	//Exit
	return 1;
}
