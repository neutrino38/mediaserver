#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "log.h"
#include "asymmetricmosaic.h"

/***********************
* AsymmetricMosaic
*	Constructor
************************/
AsymmetricMosaic::AsymmetricMosaic(Mosaic::Type type, DWORD size) : Mosaic(type,size)
{
	// Le fond noir du 1p1 est déclaré par HasBlackBackground() (lu par
	// BuildDesc) : plus aucun pixel n'est peint ici depuis la Phase 6.
}

/***********************
* AsymmetricMosaic
*	Destructor
************************/
AsymmetricMosaic::~AsymmetricMosaic()
{
}
int AsymmetricMosaic::GetWidth(int pos)
{
	int outline = 0;	
	//Check it's in the mosaic
	if (pos >= numSlots)
		return 0;

	BYTE cols = 1;
	BYTE size = 1;
	//Get values
	switch(mosaicType)
	{
		case mosaic3p4:
			/**********************************************
			*	--------------------
			*      |          |	    	|
			*      |    1	  |    2    |
			*      |_________ |_________|
			*      |	  	  | 4  | 5  |
			*      |    3	  |--- |--- |
			*      |	  	  | 6  | 7  |
			*	--------------------
			***********************************************/
			cols = 4;
			if (pos<3)
				size = 2;
			else
				size = 1;
			break;
		case mosaic1p7:
			/**********************************************
			*	----------------
			*      |	    	| 2 |
			*      |            |---|
			*      |     1      | 3 |
			*      |            |---|
			*      |	    	| 4 |
			*      |------------|---|
			*      | 5 | 6	| 7 | 8 |
			*	----------------
			***********************************************/
			cols = 4;
			if(!pos)
				size = 3;
			else
				size = 1;
			break;
		case mosaic1p4:
			/**********************************************
			*	-----------------------
			*      | 	           	   | 2 |
			*      |                   |---|
			*      |                   | 3 |
			*      |         1         |---|
			*      |   	           	   | 4 |
			*      |                   |---|
			*      |   	           	   | 5 |
			*      |-----------------------|
			*
			*      Participant size has to be different than mosaic size to keep aspect ratio
			***********************************************/
			cols = 4;
			if(!pos)
				size = 3;
			else
				size = 1;
			break;
		case mosaic1p5:
			/**********************************************
			*	-----------------
			*      |           |  2  |
			*      |     1     |---- |
			*      |	   	   |  3  |
			*      |---------- |---- |
			*      |  4  |  5  |  6  |
			*	------------------
			***********************************************/
			cols = 3;
			if(!pos)
				size = 2;
			else
				size = 1;
			break;
		case mosaic1p1:
			/**********************************************
			*	----------------
			*      |	        	|
			*      |----------------|
			*      |       |        |
			*      |   1   |   2    |
			*      |       |        |
			*      |----------------|
			*      |      	        |
			*	----------------
			***********************************************/
			cols = 4;
			size = 2;
			break;
			
		case mosaic2p8:
			/**********************************************
			*	   |-------------------|
			*      | 1  | 2  | 3  | 4  |
			*      |-------------------|
			*	   |		 |		   |
			*      |   	5    |	  6	   |
			*	   |		 |		   |
			*      |-------------------|
			*      | 7  | 8  | 9  | 10 |
			*      |-------------------|
			*
			*      Participant size has to be different than mosaic size to keep aspect ratio
			***********************************************/
			cols = 4;
			
			if(pos == 4 || pos == 5)
			{
				//outline = 10;
				size 	= 2;
			}
			else
				size = 1;
			break;
	}
	return ( (mosaicTotalWidth/cols)*size) - outline;
}
int AsymmetricMosaic::GetHeight(int pos)
{
	DWORD cols;
	int outline = 0;
	//Check it's in the mosaic
	if (pos >= numSlots)
		return 0;

	BYTE rows = 1;
	BYTE size = 1;
	//Get values
	switch(mosaicType)
	{
		case mosaic3p4:
			/**********************************************
			*	--------------------
			*      |          |	    |
			*      |    1	  |    2    |
			*      |_________ |_________|
			*      |	  | 4  | 5  |
			*      |    3	  |--- |--- |
			*      |	  | 6  | 7  |
			*	--------------------
			***********************************************/
			rows = 4;
			if (pos<3)
				size = 2;
			else
				size = 1;
			break;
		case mosaic1p7:
			/**********************************************
			*	----------------
			*      |	    | 2 |
			*      |            |---|
			*      |     1      | 3 |
			*      |            |---|
			*      |	    | 4 |
			*      |------------|---|
			*      | 5 | 6	| 7 | 8 |
			*	----------------
			***********************************************/
			rows = 4;
			if(!pos)
				size = 3;
			else
				size = 1;
			break;
		case mosaic1p4:
			/**********************************************
			*	-----------------------
			*      | 	           | 2 |
			*      |                   |---|
			*      |                   | 3 |
			*      |         1         |---|
			*      |   	           | 4 |
			*      |                   |---|
			*      |   	           | 5 |
			*      |-----------------------|
			* 
			*      Participant size has to be different than mosaic size to keep aspect ratio
			***********************************************/
			rows = 4;
			if(!pos)
				size = 4;
			else
				size = 1;
			break;
		case mosaic1p5:
			/**********************************************
			*	-----------------
			*      |           |  2  |
			*      |     1     |---- |
			*      |	       |  3  |
			*      |---------- |---- |
			*      |  4  |  5  |  6  |
			*	------------------
			***********************************************/
			rows = 3;
			if(!pos)
				size = 2;
			else
				size = 1;
			break;
		case mosaic1p1:
			/**********************************************
			*	----------------
			*      |	        |
			*      |----------------|
			*      |       |        |
			*      |   1   |   2    |
			*      |       |        |
			*      |----------------|
			*      |      	        |
			*	----------------
			***********************************************/
			// Les deux slots occupent TOUTE la hauteur (une seule rangée) : le
			// letterbox de ComputeSlotPlacement, calculé sur le ratio du slot,
			// centre l'image dedans. Avant, rows=4/size=2 bridait le slot à la
			// moitié de la hauteur, ce qui rapetissait inutilement les vignettes
			// (source 4:3 -> 479x360 au lieu de 640x480 sur une toile 1280x720).
			rows = 1;
			size = 1;
			break;
		case mosaic2p8:
			/**********************************************
			*	   |-------------------|
			*      | 1  | 2  | 3  | 4  |
			*      |-------------------|
			*	   |		 |		   |
			*      |   	5    |	  6	   |
			*	   |		 |		   |
			*      |-------------------|
			*      | 7  | 8  | 9  | 10 |
			*      |-------------------|
			*
			*      Participant size has to be different than mosaic size to keep aspect ratio
			***********************************************/
			rows = 4;
			if(pos == 4 || pos == 5)
			{
				size 	= 2;
				//outline = 10;
			}
			else
				size = 1;
			break;
	}
	return ((mosaicTotalHeight/rows)*size)-outline;
}

int AsymmetricMosaic::GetTop(int pos)
{
	BYTE index = 0;
	BYTE rows = 1;
	int outline= 0;
	//Check it's in the mosaic
	if (pos >= numSlots)
		return 0;

	//Check it's in the mosaic
	if (pos >= numSlots)
		return 0;
	
	//Get values
	switch(mosaicType)
	{
		case mosaic3p4:
			/**********************************************
			*	--------------------
			*      |          |	        |
			*      |    1	  |    2    |
			*      |_________ |_________|
			*      |	      | 4  | 5  |
			*      |    3     |--- |--- |
			*      |	      | 6  | 7  |
			*	--------------------
			***********************************************/
			switch(pos)
			{
				case 0:
				case 1:
					return 0;
				case 2:
					index = 8;
				case 3:
					index = 10;
					break;
				case 4:
					index = 11;
					break;
				case 5:
					index = 14;
					break;
				case 6:
					index = 15;
					break;
			}
			rows = 4;
			break;
		case mosaic1p7:
			/**********************************************
			*	----------------
			*      |	    	| 2 |
			*      |            |---|
			*      |     1      | 3 |
			*      |            |---|
			*      |	    	| 4 |
			*      |------------|---|
			*      | 5 | 6	| 7 | 8 |
			*	----------------
			***********************************************/
			switch(pos)
			{
				case 0:
				case 1:
					return 0;
				case 2:
					index = 7;
					break;
				case 3:
					index = 11;
					break;
				case 4:
					index = 12;
					break;
				case 5:
					index = 13;
					break;
				case 6:
					index = 14;
					break;
				case 7:
					index = 15;
					break;
			}
			rows = 4;
			break;
		case mosaic1p4:
			/**********************************************
			*		-----------------------
			*      | 	           	   | 2 |
			*      |                   |---|
			*      |                   | 3 |
			*      |         1         |---|
			*      |   	               | 4 |
			*      |                   |---|
			*      |   	               | 5 |
			*      |-----------------------|
			*
			*      Participant size has to be different than mosaic size to keep aspect ratio
			***********************************************/
			switch(pos)
			{
				case 0:
				case 1:
					return 0;
				case 2:
					index = 7;
					break;
				case 3:
					index = 11;
					break;
				case 4:
					index = 12;
					break;
				case 5:
					index = 13;
					break;
			}
			rows = 4;
			break;
		case mosaic1p5:
			/**********************************************
			*	-----------------
			*      |           |  2  |
			*      |     1     |---- |
			*      |	   	   |  3  |
			*      |---------- |---- |
			*      |  4  |  5  |  6  |
			*	------------------
			***********************************************/
			switch (pos)
			{
				case 0:
				case 1:
					return 0;
				case 2:
					index = 5;
					break;
				case 3:
					index = 6;
					break;
				case 4:
					index = 7;
					break;
				case 5:
					index = 8;
					break;
			}
			rows = 3;
			break;
		case mosaic2p8:
			/**********************************************
			*	   |-------------------|
			*      | 1  | 2  | 3  | 4  |
			*      |-------------------|
			*	   |		 |		   |
			*      |   	5    |	  6	   |
			*	   |		 |		   |
			*      |-------------------|
			*      | 7  | 8  | 9  | 10 |
			*      |-------------------|
			*
			*      Participant size has to be different than mosaic size to keep aspect ratio
			***********************************************/
			switch (pos)
			{
				case 0:
				case 1:
				case 2:
				case 3:
					return 0;
				case 4:
					index = 7;
					//outline = 5;
					break;
				case 5:
					index = 7;
					//outline = 5;
					break;
				case 6:
					index = 12;
					break;
				case 7:
					index = 12;
					break;
				case 8:
					index = 12;
					break;
				case 9:
					index = 12;
					break;
			}
			rows = 4;
			break;
		case mosaic1p1:
			/**********************************************
			*	----------------
			*      |	        |
			*      |----------------|
			*      |       |        |
			*      |   1   |   2    |
			*      |       |        |
			*      |----------------|
			*      |      	        |
			*	----------------
			***********************************************/
			// Slots pleine hauteur (cf. GetHeight) : le centrage vertical de
			// l'image est assuré par le letterbox du slot, plus par un décalage
			// en dur de la mosaïque.
			return 0;
	}
	//Get row
	int i = index/rows;
	//Get row heigth
	int mosaicHeight = mosaicTotalHeight/rows;
	// if we have an odd height , we round to the smallest even height.
	if (mosaicHeight %2)
			mosaicHeight=mosaicHeight-1;
	//Calculate top
	return (mosaicHeight*i)+outline;
}
int AsymmetricMosaic::GetLeft(int pos)
{
	//Check it's in the mosaic
	if (pos >= numSlots)
		return 0;

	//Check it's in the mosaic
	if (pos >= numSlots)
		return 0;

	BYTE index;
	BYTE cols;
	int outline =0;
	
	//Get values
	switch(mosaicType)
	{
		case mosaic3p4:
			/**********************************************
			*	--------------------
			*      |          |	        |
			*      |    1	  |    2    |
			*      |_________ |_________|
			*      |	      | 4  | 5  |
			*      |    3 	  |--- |--- |
			*      |	  	  | 6  | 7  |
			*	--------------------
			***********************************************/
			switch(pos)
			{
				case 0:
				case 2:
					return 0;
				case 1:
					index = 2;
					break;
				case 3:
					index = 10;
					break;
				case 4:
					index = 11;
					break;
				case 5:
					index = 14;
					break;
				case 6:
					index = 15;
					break;
			}
			cols = 4;
			break;
		case mosaic1p7:
			/**********************************************
			*	----------------
			*      |	    	| 2 |
			*      |            |---|
			*      |     1      | 3 |
			*      |            |---|
			*      |	    	| 4 |
			*      |------------|---|
			*      | 5 | 6	| 7 | 8 |
			*	----------------
			***********************************************/
			switch(pos)
			{
				case 0:
					return 0;
				case 1:
					index = 3;
					break;
				case 2:
					index = 7;
					break;
				case 3:
					index = 11;
					break;
				case 4:
					index = 12;
					break;
				case 5:
					index = 13;
					break;
				case 6:
					index = 14;
					break;
				case 7:
					index = 15;
					break;
			}
			cols = 4;
			break;
		case mosaic1p4:
			/**********************************************
			*	-----------------------
			*      | 	           | 2 |
			*      |                   |---|
			*      |                   | 3 |
			*      |         1         |---|
			*      |   	           | 4 |
			*      |                   |---|
			*      |   	           | 5 |
			*      |-----------------------|
			*
			*      Participant size has to be different than mosaic size to keep aspect ratio
			***********************************************/
			switch(pos)
			{
				case 0:
					return 0;
				case 1:
					index = 3;
					break;
				case 2:
					index = 7;
					break;
				case 3:
					index = 11;
					break;
				case 4:
					index = 12;
					break;
				case 5:
					index = 13;
					break;
			}
			cols = 4;
			break;
		case mosaic1p5:
			/**********************************************
			*	-----------------
			*      |           |  2  |
			*      |     1     |---- |
			*      |	   	   |  3  |
			*      |---------- |---- |
			*      |  4  |  5  |  6  |
			*	------------------
			***********************************************/
			switch (pos)
			{
				case 0:
					return 0;
				case 1:
					index = 2;
					break;
				case 2:
					index = 5;
					break;
				case 3:
					index = 6;
					break;
				case 4:
					index = 7;
					break;
				case 5:
					index = 8;
					break;
			}
			cols = 3;
			break;
		case mosaic2p8:
				/**********************************************
			*	   |-------------------|
			*      | 1  | 2  | 3  | 4  |
			*      |-------------------|
			*	   |		 |		   |
			*      |   	5    |	  6	   |
			*	   |		 |		   |
			*      |-------------------|
			*      | 7  | 8  | 9  | 10 |
			*      |-------------------|
			*
			*      Participant size has to be different than mosaic size to keep aspect ratio
			***********************************************/
			switch (pos)
			{
				case 0:
				case 6:
				case 4:
					return 0;
				case 5:
					index = 6;
					//outline =5;
					break;
				case 1:
				case 7:
					index = 5;
					break;
				case 2:
				case 8:
					index = 6;
					break;
				case 3:
				case 9:
					index = 7;
					break;
			}
			cols = 4;
			break;
		case mosaic1p1:
			/**********************************************
			*	----------------
			*      |	        |
			*      |----------------|
			*      |       |        |
			*      |   1   |   2    |
			*      |       |        |
			*      |----------------|
			*      |      	        |
			*	----------------
			***********************************************/
			if(!pos)
					return 0;
			index = 1;
			cols = 2;
			break;
	}

	int i = index/cols;
	int j = index - i*cols;
	int mosaicWidth = mosaicTotalWidth/cols;
	// if we have an odd width , we round to the smallest even width.
	if (mosaicWidth %2)
			mosaicWidth=mosaicWidth-1;
	//Start filling from the end to not cause overlap
	return (mosaicWidth*j)+outline;
}
