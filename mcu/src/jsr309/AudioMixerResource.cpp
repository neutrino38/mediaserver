#include "log.h"
#include "AudioMixerResource.h"

AudioMixerResource::AudioMixerResource(std::wstring &tag)
{
	//Store tag
	this->tag = tag;

	//Not inited
	inited = false;

	//Init part counter
	maxId = 1;
}

AudioMixerResource::~AudioMixerResource()
{
	//Check if ended properly
	if (inited)
		//End!!
		End();
}

int AudioMixerResource::Init()
{

	Log("-Init AudioMixerResource\n");

	//Init audio mixer without vad
	inited = mixer.Init(false);

	//Exti
	return inited;
}

int AudioMixerResource::CreatePort(std::wstring &tag)
{
	Log(">Create AudioMixerResourcePort\n");

	//SI no tamos iniciados pasamos
	if (!inited)
		return Error("Not inited\n");

	//Obtenemos el id
	int portId = maxId++;

	//Y el de audio
	if (!mixer.CreateMixer(portId))
		return Error("Couldn't set audio mixer\n");

	//Create the audio port
	std::shared_ptr<Port> port = std::make_shared<Port>(tag);

	//Init encoder and decoder
	port->encoder.Init(mixer.GetInput(portId));
	port->decoder.Init(mixer.GetOutput(portId));

	//Init mixer id
	mixer.InitMixer(portId,0);

	//Lo insertamos en el map
	ports[portId] = port;

	Log("<CreateParticipant [%d]\n",portId);

	return portId;
}

int AudioMixerResource::SetPortCodec(int portId,AudioCodec::Type codec)
{
	//Find port
	Ports::iterator it = ports.find(portId);

	//Check present
	if (it == ports.end())
		//Error
		return Error("Audio port not found\n");

	//LO obtenemos
	std::shared_ptr<Port> port = it->second;

	//Set codec
	port->encoder.SetCodec(codec);
	
	//Start
	return 1;
}

int AudioMixerResource::DeletePort(int portId)
{
	//Find port
	Ports::iterator it = ports.find(portId);

	//Check present
	if (it == ports.end())
		//Error
		return Error("Audio port not found\n");

	//LO obtenemos
	std::shared_ptr<Port> port = it->second;
	
	//Remove it from list
	ports.erase(it);

	//End mixer id
	mixer.EndMixer(portId);

	//End encoder and decoder
	port->encoder.End();
	port->decoder.End();

	//End remove
	mixer.DeleteMixer(portId);

	//Le shared_ptr local détruit le Port en sortie de portée (après End) : un
	//weak_ptr `joined` d'un listener encore attaché expire alors proprement (C-13).

	//OK
	return 1;
}

int AudioMixerResource::End()
{
	//For each port
	for (Ports::iterator it = ports.begin(); it!= ports.end();++it)
	{
		//Get port
		std::shared_ptr<Port> port = it->second;

		//End encoder and decoder
		port->encoder.End();
		port->decoder.End();
	}

	//Clear list (les shared_ptr détruisent les Port)
	ports.clear();

	//End mixer
	mixer.End();

	return 1;
}

std::shared_ptr<Joinable> AudioMixerResource:: GetJoinable(int portId)
{
	//Find port
	Ports::iterator it = ports.find(portId);

	//Check present
	if (it == ports.end())
        {
            Error("Audio port not found\n");
		//Error
		return nullptr;
        }

	//shared_ptr aliasing : partage le compteur du Port mais pointe sur son
	//`encoder`. Le weak_ptr `joined` du listener attaché expire quand le Port
	//est détruit (DeletePort/End) — C-13, lien A.
	return std::shared_ptr<Joinable>(it->second, &it->second->encoder);
}

int AudioMixerResource::Attach(int portId,const std::shared_ptr<Joinable> & join)
{
	//Find port
	Ports::iterator it = ports.find(portId);

	//Check present
	if (it == ports.end())
		//Error
		return Error("Audio port not found\n");

	//LO obtenemos
	std::shared_ptr<Port> port = it->second;

	//Init
	//OK
	return port->decoder.Attach(join);
}

int AudioMixerResource::Dettach(int portId)
{
	//Find port
	Ports::iterator it = ports.find(portId);

	//Check present
	if (it == ports.end())
		//Error
		return Error("Audio port not found\n");

	//LO obtenemos
	std::shared_ptr<Port> port = it->second;

	//OK
	return port->decoder.Dettach();
}
