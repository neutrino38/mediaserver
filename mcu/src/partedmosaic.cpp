#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "log.h"
#include "partedmosaic.h"
#include <stdexcept>

/***********************
* PartedMosaic
*	Constructor
************************/
PartedMosaic::PartedMosaic(Type type, DWORD size) : Mosaic(type,size)
{
	//Divide mosaic
	switch(type)
	{
		case mosaic1x1:
			//Set num rows and cols
			mosaicCols = 1;
			mosaicRows = 1;
			break;
		case mosaic2x2:
			//Set num rows and cols
			mosaicCols = 2;
			mosaicRows = 2;
			break;
		case mosaic3x3:
			//Set num rows and cols
			mosaicCols = 3;
			mosaicRows = 3;
			break;
		case mosaic4x4:
			//Set num rows and cols
			mosaicCols = 4;
			mosaicRows = 4;
			break;
		default:
			//Inatteignable : Mosaic::CreateMosaic filtre les types en amont et rend
			//NULL. Lever PAR VALEUR malgré tout (et non `throw new`, un pointeur que
			//nul ne peut attraper) pour qu'un appel direct au constructeur reste
			//diagnosticable au lieu de terminer le processus.
			throw std::runtime_error("Unknown parted mosaic type\n");

	}

	mosaicWidth = (int) mosaicTotalWidth/mosaicCols;
	// if we have an odd width , we round to the smallest even width.
	if (mosaicWidth %2)
			mosaicWidth=mosaicWidth-1;
	mosaicHeight = (int) mosaicTotalHeight/mosaicRows;
	// if we have an odd height , we round to the smallest even height.
	if (mosaicHeight %2)
			mosaicHeight=mosaicHeight-1;
}

/***********************
* PartedMosaic
*	Destructor
************************/
PartedMosaic::~PartedMosaic()
{
}

int PartedMosaic::GetWidth(int pos)
{
	//Check it's in the mosaic
	if (pos >= numSlots)
		return 0;

	//Get widht
	return mosaicWidth;
}
int PartedMosaic::GetHeight(int pos)
{
	//Check it's in the mosaic
	if (pos >= numSlots)
		return 0;

	//Get widht
	return mosaicHeight;
}
int PartedMosaic::GetTop(int pos)
{
	//Get slot position in mosaic
	int i = pos / mosaicCols;

	//Get offsets
	return mosaicHeight*i;
}
int PartedMosaic::GetLeft(int pos)
{
	//Get slot position in mosaic
	int i = pos / mosaicCols;
	int j = pos - i*mosaicCols;
	//Get offsets
	return mosaicWidth*j;
}
