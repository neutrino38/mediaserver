#ifndef _MOSAIC_H_
#define _MOSAIC_H_
#include "config.h"
#include "video.h"
#include "overlay.h"
#include "mosaiccompositor.h"
#include "vad.h"
#include <map>
#include <memory>
#include <vector>

class Mosaic
{
public:
	static const int NotShown = -1;
	static const int NotFound = -2;

	static const int SlotFree     = 0;
	static const int SlotLocked   = -1;
	static const int SlotVAD      = -2;
	static const int SlotReset    = -3; // For internal use only

	// Liseré noir (px) réservé autour de l'image de chaque slot : le slot utile
	// est rétréci d'autant de chaque côté et le graphe pad l'image en noir.
	// (constexpr : implicitement inline en C++17, gtest la lie par référence.)
	static constexpr int SlotBorder = 2;
	
	typedef enum
	{
		mosaic1x1	= 0,
		mosaic2x2	= 1,
		mosaic3x3	= 2,
		mosaic3p4	= 3,
		mosaic1p7	= 4,
		mosaic1p5	= 5,
		mosaic1p1	= 6,
		mosaicPIP1	= 7,
		mosaicPIP3	= 8,
		mosaic4x4	= 9,
		mosaic1p4	= 10,
		mosaic2p8	= 11,
	} Type;

	class PartInfo
	{
	    public:
		PartInfo() 
		{
		    vadLevel = 0;
		    kickable = false;
		    eligible = false;
		}
		int vadLevel;
		bool kickable;
		bool eligible;
	};

public:
	Mosaic(Type type,DWORD size);
	virtual ~Mosaic();

	int GetWidth()		{ return mosaicTotalWidth;}
	int GetHeight()		{ return mosaicTotalHeight;}
	int HasChanged()	{ return mosaicChanged; }
	void Reset()		{ mosaicChanged = true; }

	// Composite de la mosaïque : composition par le graphe avfilter
	// (MosaicCompositor) à partir des trames mémorisées par slot, avec cache
	// intra-tick. Le chemin BYTE* historique a disparu en Phase 6.
	PictPtr GetPict();
	// Mémorise la trame du slot (GPU conservée telle quelle depuis la Phase 5)
	// et l'aspect du slot ; la composition est faite par le graphe avfilter.
	int Update(int index, const PictPtr& pic);
	// Slot vidé : plus de trame mémorisée, le fond redevient visible.
	int Clean(int index)
	{
		ClearSlotFrame(index);
		SetChanged();
		return 1;
	}

	int AddParticipant(int id);
	int HasParticipant(int id);
	int RemoveParticipant(int id);
	int SetSlot(int num,int id);
	int SetSlot(int num,int id,QWORD blockedUntil);
	QWORD GetBlockingTime(int num);

	int GetPosition(int id);
	int GetVADPosition();
	int* GetPositions();
	int* GetSlots();
	int GetNumSlots();
	void SetSlots(int *slots,int num);
	bool IsFixed(DWORD pos);

	void Dump();

	int GetVADParticipant();
	int SetVADParticipant(int id,QWORD blockedUntil);

	static int GetNumSlotsForType(Type type);
	static Mosaic* CreateMosaic(Type type,DWORD size);

	int SetOverlayImage(int id, const char* filename);
	int SetOverlaySVG(int id, const char* svg);
	int SetOverlayTXT(int id, const char *msg,int scriptCode);
	int ResetOverlay(int id);

	int UpdateParticipantInfo(int id, int vadLevel);
	int CalculatePositions();
	void KeepAspectRatio(bool keep) { keepAspect = keep; }
	
	/** Compute aspect ratio of a picture and take into acconnt
	 *  the case of non square pixel in CIF / SIF formats
	 *  @param imgWidth width of picture
	 *  @param imgHeight height of picture
	 *  @return aspect ratio of picture
	 **/
	DWORD ComputeAspectRatio(DWORD imgWidth, DWORD imgHeight);
	
        /**
         * move all the participants overlays *nd the mosaic overlay from other
	 * to the current mosaic. After this call overlays of the "other" mosaic
	 * are attached to "this" mosaic.
	 *
         * @param source mosaic.
         */
	void MoveOverlays(Mosaic *other);

protected:
	virtual int GetWidth(int pos) = 0;
	virtual int GetHeight(int pos) = 0;
	virtual int GetTop(int pos) = 0;
	virtual int GetLeft(int pos) = 0;

	// Fond noir (mosaic1p1) au lieu du gris neutre. cf. HasBlackBackground du
	// constructeur d'AsymmetricMosaic (peint-en-noir).
	virtual bool HasBlackBackground() const { return false; }
	// Slot toujours étiré plein cadre, aspect ignoré (PIPMosaic pos==0, fidèle à
	// l'étirement historique du slot principal PIP).
	virtual bool StretchSlot(int pos) const { return false; }

	// Liseré effectif du slot : SlotBorder, ou 0 si le slot est trop petit pour
	// en réserver un (garde-fou ; tous les slots réels font >= ~180 px).
	int GetSlotBorder(int pos);

	// Calcule la taille effective de l'image (letterbox/pillarbox) et son
	// décalage dans le slot, d'après ComputeAspectRatio et keepAspect. Le
	// placement se fait dans le slot UTILE (liseré déduit de chaque côté) ;
	// dx/dy incluent le liseré. Factorise l'arithmétique dupliquée des
	// Update(BYTE*) (partedmosaic / asymmetricmosaic).
	void ComputeSlotPlacement(int pos, int inW, int inH, bool keepAspect,
	                          int& outW, int& outH, int& dx, int& dy);

	// Construit la description du graphe de composition (géométrie + placement
	// letterbox de chaque slot ACTIF, overlays participant/mosaïque). Matérialise
	// au passage les Pict RGBA des overlays dans slotOverlayPicts/mosaicOverlayPict
	// (rendu Overlay mis en cache, re-rendu seulement si contenu/taille changent).
	// cf. mosaic_avfilter_plan.md §3.
	MosaicGraphDesc BuildDesc();

	// Fond de la mosaïque (Pict WxH, généré une fois : couleur/taille fixes).
	// Gris neutre (Y=U=V=128) ou noir si HasBlackBackground() (mosaic1p1).
	const PictPtr& GetBackground();

	// Retire la trame mémorisée d'un slot (slot vide = pas de branche dans le
	// graphe, le fond reste visible). À appeler depuis Clean() des dérivées.
	void ClearSlotFrame(int pos)
	{
		if (pos >= 0 && pos < (int) slotFrames.size())
			slotFrames[pos].reset();
	}

protected:
	void SetChanged()	{ mosaicChanged = true; compositeValid = false; }

protected:
	typedef std::map<int,int> Participants;
	typedef std::map<int,PartInfo> ParticipantInfos;

protected:
	Participants participants;
	ParticipantInfos partVad;
	std::map<int,std::unique_ptr<Overlay>> overlays;
	int mosaicChanged;

	// information on whether slot is locked, free, fixed (= id of participant), vad
	int *mosaicSlots;

	// association between position and ids
	int *mosaicPos;
	QWORD *mosaicSlotsBlockingTime;
	QWORD *oldTimes;
	int numSlots;
	int vadParticipant;

	int 	mosaicTotalWidth;
	int 	mosaicTotalHeight;
	Type	mosaicType;

	std::unique_ptr<Overlay> overlay;

	bool  keepAspect;
	DWORD ratio; /* ratio * 1000 */

protected:
	int GetNextFreeSlot(int id);

	// --- Chemin avfilter (migration graphe unique, cf. mosaic_avfilter_plan.md) ---
	// Dernière trame connue par slot (nullptr = slot vide, pas de branche).
	std::vector<PictPtr> slotFrames;
	// Aspect à conserver, mémorisé par slot au moment du Update (corrige la course
	// bénigne du drapeau global keepAspect écrasé à chaque Update).
	std::vector<bool>    slotKeepAspect;
	PictPtr              background;   // Pict de fond WxH (généré paresseusement)
	PictPtr              composite;    // cache du dernier composite
	bool                 compositeValid = false; // composite à jour (invalidé par SetChanged)
	MosaicCompositor     compositor;   // matérialisation du graphe
	// Pict RGBA des overlays, remplis par BuildDesc (alignés sur desc.slots ;
	// nullptr si le slot n'a pas d'overlay), consommés par GetPict -> Compose.
	std::vector<PictPtr> slotOverlayPicts;
	PictPtr              mosaicOverlayPict;
};

#endif
