#ifndef _PARTEDMOSAIC_H_
#define _PARTEDMOSAIC_H_

#include "mosaic.h"

// Grilles régulières (1x1, 2x2, 3x3, 4x4). Depuis la Phase 6 (avfilter), les
// dérivées de Mosaic ne portent plus que la GÉOMÉTRIE des slots : la
// composition est faite par MosaicCompositor à partir de BuildDesc().
class PartedMosaic:
	public Mosaic
{
public:
	PartedMosaic(Mosaic::Type type, DWORD size);
	virtual ~PartedMosaic();

protected:
	virtual int GetWidth(int pos);
	virtual int GetHeight(int pos);
	virtual int GetTop(int pos);
	virtual int GetLeft(int pos);
private:
	int     mosaicCols;
	int     mosaicRows;
	int     mosaicWidth;
	int     mosaicHeight;
};

#endif
