#ifndef _ASYMMETRICMOSAIC_H_
#define _ASYMMETRICMOSAIC_H_

#include "mosaic.h"

// Dispositions asymétriques (1p1, 3p4, 1p7, 1p5, 1p4, 2p8). Depuis la Phase 6
// (avfilter), les dérivées de Mosaic ne portent plus que la GÉOMÉTRIE des
// slots : la composition est faite par MosaicCompositor via BuildDesc().
class AsymmetricMosaic:
	public Mosaic
{
public:
	AsymmetricMosaic(Type type, DWORD size);
	virtual ~AsymmetricMosaic();

protected:
	// mosaic1p1 est peint en noir (cf. constructeur).
	virtual bool HasBlackBackground() const { return mosaicType == mosaic1p1; }
	virtual int GetWidth(int pos);
	virtual int GetHeight(int pos);
	virtual int GetTop(int pos);
	virtual int GetLeft(int pos);
};
#endif
