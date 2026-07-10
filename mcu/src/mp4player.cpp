#include "mp4player.h"
#include "log.h"
#include "medkit/codecs.h"

MP4Player::MP4Player() : streamer(this)
{
	//audioDecoder/videoDecoder sont des unique_ptr : déjà nuls par défaut.
}

MP4Player::~MP4Player()
{
	//Arrête le thread de lecture (streamer) AVANT que les unique_ptr des
	//décodeurs ne se détruisent : onRTPPacket tourne sur ce thread et utilise
	//audioDecoder/videoDecoder (M-3). Stop() joint le worker avant de revenir.
	Stop();
}

int MP4Player::Init(AudioOutput *audioOutput,VideoOutput *videoOutput,TextOutput *textOutput)
{
	//Sava
	this->audioOutput = audioOutput;
	this->videoOutput = videoOutput;
	this->textOutput = textOutput;
	return 0;
}

int MP4Player::Play(const char* filename,bool loop)
{
	Log("-MP4Player play [\"%s\"]\n",filename);

	//Stop just in case
	streamer.Close();

	//Open file
	if (!streamer.Open(filename))
		//Error
		return Error("Error opening mp4 file");

	//Save loop value
	this->loop = loop;

	//Open audio codec
	if (streamer.HasAudioTrack())
		//Create audio codec
		audioDecoder.reset(AudioCodecFactory::CreateDecoder(streamer.GetAudioCodec()));

	//Open video codec
	if (streamer.HasVideoTrack())
		//Create audio codec
		videoDecoder.reset(VideoCodecFactory::CreateDecoder(streamer.GetVideoCodec()));
		
	//Start playback
	return streamer.Play();
}

int MP4Player::Stop()
{
	Log("-MP4Player stop\n");

	//Do not loop anymore
	loop = false;

	//Stop
	streamer.Stop();
	
	//Close
	streamer.Close();

	return 1;
}

int MP4Player::End()
{
	return 0;
}

void MP4Player::onTextFrame(TextFrame &text)
{
	Log("-On TextFrame [\"%ls\"]\n",text.GetWChar());

	//Check textOutput
	if (textOutput)
		//Publish it
		textOutput->SendFrame(text);
}

void MP4Player::onRTPPacket(RTPPacket &packet)
{
	SWORD buffer[1024];
	DWORD bufferSize = 1024;
	//Get data
	BYTE *data = packet.GetMediaData();
	//Get leght
	DWORD len = packet.GetMediaLength();
	//Get mark
	bool mark = packet.GetRTPHeader()->m;
	
	//Depending on the media
	switch (packet.GetMedia())
	{
		case MediaFrame::Audio:
			//Check decoder
			if (!audioDecoder || !audioOutput)
				//Do nothing
				return;

			//Decode it
			len = audioDecoder->Decode(data,len,buffer,bufferSize);

			//Play it
			audioOutput->PlayBuffer(buffer,len,0);

			break;
		case MediaFrame::Video:
			//Check decoder
			if (!videoDecoder || !videoOutput)
				//Do nothing
				return;
			
			//Decode packet
			if(!videoDecoder->DecodePacket(data,len,false,mark))
				//Error
				return;
			//Check if it is last
			if(mark)
			{
				//Get dimensions
				DWORD width = videoDecoder->GetWidth();
				DWORD height= videoDecoder->GetHeight();
				//Set it
				videoOutput->SetVideoSize(width,height);
				//Set decoded frame
				videoOutput->NextFrame(videoDecoder->GetFrame());
			}
			break;
	}

}

void MP4Player::onMediaFrame(MediaFrame &frame)
{
	//Do nothing now
}

void MP4Player::onEnd()
{
	//If in loop
	if (loop)
		//Play again
		streamer.Play();
}
