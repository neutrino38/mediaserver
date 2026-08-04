/* 
 * File:   appmixer.cpp
 * Author: Sergio
 * 
 * Created on 15 de enero de 2013, 15:38
 */

#include "appmixer.h"
#include "log.h"

AppMixer::AppMixer()
{
	//No output
	output = NULL;
}

int AppMixer::Init(VideoOutput* output)
{
	//Set output
	this->output = output;
	return 0;
}

int AppMixer::DisplayImage(const char* filename)
{
	Log("-DisplayImage [\"%s\"]\n",filename);

	//Load image
	logo = Pict::Load(filename);
	if (!logo)
		//Error
		return Error("-Error loading file");

	//Check output
	if (!output)
		//Error
		return Error("-No output");

	//Set size
	output-> SetVideoSize(logo->GetWidth(),logo->GetHeight());
	//Set image
	output->NextFrame(logo);

	//Everything ok
	return true;
}

int AppMixer::DisplayImage(const PictPtr& p_logo)
{
	Log("-DisplayImage Logo \n");
	// Partage la trame (immuable) ; nullptr => rien à afficher.
	logo = (p_logo && p_logo->GetAVFrame()) ? p_logo : nullptr;

	//Check output
	if (!output)
		//Error
		return Error("-No output");

	//Set image if any
	if (logo)
	{
		output-> SetVideoSize(logo->GetWidth(),logo->GetHeight());
		output->NextFrame(logo);
	}

	//Everything ok
	return true;
}

int AppMixer::End()
{
	//Reset output
	output = NULL;
	return 0;
}
