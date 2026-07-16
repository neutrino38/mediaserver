#ifndef _ASYMMETRICMOSAIC_H_
#define _ASYMMETRICMOSAIC_H_

#include "mosaic.h"
#include "framescaler.h"

class AsymmetricMosaic:
	public Mosaic
{
public:
	AsymmetricMosaic(Type type, DWORD size);
	virtual ~AsymmetricMosaic();

	virtual int Update(int index,BYTE *frame,int width,int heigth);
	virtual int Clean(int index);
protected:
	// mosaic1p1 est peint en noir (cf. constructeur).
	virtual bool HasBlackBackground() const { return mosaicType == mosaic1p1; }
	virtual int GetWidth(int pos);
	virtual int GetHeight(int pos);
	virtual int GetTop(int pos);
	virtual int GetLeft(int pos);
};
#endif
