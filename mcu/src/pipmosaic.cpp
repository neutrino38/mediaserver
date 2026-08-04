#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "log.h"
#include "pipmosaic.h"

/***********************
* PIPMosaic
*	Constructor
************************/
PIPMosaic::PIPMosaic(Mosaic::Type type, DWORD size) : Mosaic(type,size)
{
	// Toute la géométrie vient de la base ; l'image « du dessous » (under) et
	// ses blits ont disparu avec le chemin BYTE* (Phase 6) — le slot 0 plein
	// cadre et les incrustations sont composés par le graphe avfilter.
}

/***********************
* PIPMosaic
*	Destructor
************************/
PIPMosaic::~PIPMosaic()
{
}

int PIPMosaic::GetWidth(int pos)
{
	//Check it's in the mosaic
	if (pos+1>numSlots)
		//Exit
		return 0;
	//Main
	if (!pos)
		return mosaicTotalWidth;

	//If only 2 PIP
	if (numSlots==2)
		return SIZE4MUL(mosaicTotalWidth/4);

	return SIZE4MUL(mosaicTotalWidth/5);
}

int PIPMosaic::GetHeight(int pos)
{
	//Check it's in the mosaic
	if (pos+1>numSlots)
		//Exit
		return 0;
	//Main
	if(!pos)
		return mosaicTotalHeight;
	//If only 2 PIP
	if (numSlots==2)
		return SIZE4MUL(mosaicTotalHeight/4);
	return SIZE4MUL(mosaicTotalHeight/5);
}
int PIPMosaic::GetTop(int pos)
{
	//Check it's in the mosaic
	if (pos+1>numSlots)
		//Exit
		return 0;
	//Main
	if (!pos)
		return 0;
	//Calculate pip participant size
	DWORD mosaicHeight		= SIZE4MUL(mosaicTotalHeight/5);

	// if we have an odd height , we round to the smallest even height.
	if (mosaicHeight %2)
			mosaicHeight=mosaicHeight-1;

	//Get top position
	return SIZE4MUL(mosaicTotalHeight-mosaicHeight-mosaicHeight/2);
}
int PIPMosaic::GetLeft(int pos)
{
	//Check it's in the mosaic
	if (pos+1>numSlots)
		//Exit
		return 0;
		//Main
	if (!pos)
		return 0;
	//Calculate pip participant size
	DWORD mosaicWidth		= SIZE4MUL(mosaicTotalWidth/5);

	// if we have an odd width , we round to the smallest even width.
	if (mosaicWidth %2)
			mosaicWidth=mosaicWidth-1;

	//Get empty space between PIP
	DWORD intraWidth = mosaicWidth/2;

	return intraWidth + (intraWidth+mosaicWidth)*(pos-1);
}
