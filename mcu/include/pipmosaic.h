#ifndef _PIPMOSAIC_H_
#define _PIPMOSAIC_H_

#include "mosaic.h"

// Picture-in-picture (PIP1, PIP3) : slot 0 plein cadre étiré, incrustations
// par-dessus. Depuis la Phase 6 (avfilter), les dérivées de Mosaic ne portent
// plus que la GÉOMÉTRIE des slots : la composition est faite par
// MosaicCompositor via BuildDesc(). NB : les vignettes se superposent, donc le
// mode GPU replie en CPU (liseré peint dans le fond, cf. MosaicCompositor).
class PIPMosaic:
	public Mosaic
{
public:
	PIPMosaic(Mosaic::Type type, DWORD size);
	virtual ~PIPMosaic();

protected:
	// Le slot principal (pos 0) est toujours étiré plein cadre (fidèle à l'existant).
	virtual bool StretchSlot(int pos) const { return pos == 0; }
	virtual int GetWidth(int pos);
	virtual int GetHeight(int pos);
	virtual int GetTop(int pos);
	virtual int GetLeft(int pos);
};
#endif
