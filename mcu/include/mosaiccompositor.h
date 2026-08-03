#ifndef _MOSAICCOMPOSITOR_H_
#define _MOSAICCOMPOSITOR_H_

#include <video.h>
#include <vector>
extern "C"
{
#include <libavfilter/avfilter.h>
}

// Description d'un slot ACTIF de la mosaïque (= slot pour lequel une trame est
// mémorisée). Sert de clé de (re)configuration du graphe : tout changement
// structurel (géométrie, taille/format d'entrée, présence d'overlay) reconstruit
// le graphe. Un simple changement de contenu (nouvelle trame) ne reconstruit rien.
struct MosaicSlotDesc
{
	int  pos       = 0;      // index de slot (traçage/logs)
	int  x         = 0;      // position de la vignette (GetLeft + décalage letterbox)
	int  y         = 0;      // position de la vignette (GetTop + décalage letterbox)
	int  w         = 0;      // largeur effective de la vignette (rzWidth)
	int  h         = 0;      // hauteur effective de la vignette (rzHeight)
	int  inW       = 0;      // largeur de la trame d'entrée (clé de reconfig)
	int  inH       = 0;      // hauteur de la trame d'entrée (clé de reconfig)
	int  inFmt     = 0;      // format de la trame d'entrée (AVPixelFormat, clé de reconfig)
	AVBufferRef* hwFramesCtx = nullptr; // ctx trames VAAPI de l'entrée (non possédé), clé de reconfig
	bool hasOverlay = false; // overlay participant à empiler sur ce slot
	int  ovX       = 0;      // position de l'overlay participant (= GetLeft du slot)
	int  ovY       = 0;      // position de l'overlay participant (= GetTop du slot)
	int  ovW       = 0;      // largeur de l'overlay rgba (== largeur du slot)
	int  ovH       = 0;      // hauteur de l'overlay rgba (== hauteur du slot)

	bool operator==(const MosaicSlotDesc& o) const
	{
		return pos == o.pos && x == o.x && y == o.y && w == o.w && h == o.h &&
		       inW == o.inW && inH == o.inH && inFmt == o.inFmt &&
		       hwFramesCtx == o.hwFramesCtx && hasOverlay == o.hasOverlay &&
		       ovX == o.ovX && ovY == o.ovY && ovW == o.ovW && ovH == o.ovH;
	}
	bool operator!=(const MosaicSlotDesc& o) const { return !(*this == o); }
};

// Description complète du graphe de composition d'une mosaïque. Comparée
// structurellement à chaque tick : reconstruction UNIQUEMENT si elle change.
struct MosaicGraphDesc
{
	int  width  = 0;              // largeur du composite (mosaicTotalWidth)
	int  height = 0;              // hauteur du composite (mosaicTotalHeight)
	bool wantGPU = false;         // souhait GPU (le compositor peut replier CPU)
	bool blackBackground = false; // fond noir (mosaic1p1) au lieu du gris neutre
	bool hasMosaicOverlay = false;// overlay plein cadre présent
	std::vector<MosaicSlotDesc> slots; // slots ACTIFS uniquement, ordre = ordre Z

	bool operator==(const MosaicGraphDesc& o) const
	{
		return width == o.width && height == o.height && wantGPU == o.wantGPU &&
		       blackBackground == o.blackBackground &&
		       hasMosaicOverlay == o.hasMosaicOverlay && slots == o.slots;
	}
	bool operator!=(const MosaicGraphDesc& o) const { return !(*this == o); }
};

// Matérialise une MosaicGraphDesc en AVFilterGraph PERSISTANT (scale +
// composition overlay) et gère le cycle push/pull par tick. Généralisation
// N-entrées de VideoRescaler : mêmes idiomes (reconfiguration paresseuse,
// avfilter_graph_parse_ptr). Côté mcu (comme VideoRescaler) ; non copiable
// (possède un graphe ffmpeg). NON thread-safe : tous les accès se font sous le
// verrou du thread MixVideo (cf. mosaic_avfilter_plan.md §4).
class MosaicCompositor
{
public:
	MosaicCompositor();
	~MosaicCompositor();

	MosaicCompositor(const MosaicCompositor&)            = delete;
	MosaicCompositor& operator=(const MosaicCompositor&) = delete;

	// (Re)construit le graphe si desc != description courante (sinon no-op).
	// Applique la politique GPU/CPU avec repli automatique. false = échec
	// définitif (l'appelant renonce à composer ce tick).
	bool Configure(const MosaicGraphDesc& desc);

	// Un tick de composition : pousse la trame de chaque slot actif, le fond,
	// les overlays (tous au même pts, tick monotone), puis tire le sink une fois.
	// slotFrames[i]/slotOverlays[i] sont alignés sur desc.slots[i] (slotOverlays[i]
	// n'est consulté que si desc.slots[i].hasOverlay). background est obligatoire ;
	// mosaicOverlay est requis ssi desc.hasMosaicOverlay.
	// Retour : composite dans un Pict NEUF (nullptr en cas d'échec).
	PictPtr Compose(const std::vector<PictPtr>& slotFrames,
	                const std::vector<PictPtr>& slotOverlays,
	                const PictPtr& background,
	                const PictPtr& mosaicOverlay);

	void Release();

private:
	// Matérialise cur -> AVFilterGraph. gpu=true : chaîne VAAPI (Phase 5).
	bool BuildGraph(bool gpu);
	// Pousse une référence de 'pic' (conteneur éphémère au pts donné) dans src.
	bool PushInput(AVFilterContext* src, const PictPtr& pic, int64_t pts);

	AVFilterGraph*   graph   = nullptr;
	AVFilterContext* sinkCtx = nullptr;
	AVFilterContext* bgSrc   = nullptr;
	std::vector<AVFilterContext*> slotSrcs;   // 1 par desc.slots (aligné)
	std::vector<AVFilterContext*> ovrSrcs;    // 1 par desc.slots (nullptr si !hasOverlay)
	AVFilterContext* mosaicOvrSrc = nullptr;

	MosaicGraphDesc  cur;                      // description du graphe courant
	bool             curGPU    = false;        // mode effectif du graphe courant
	bool             gpuBroken  = false;       // échec GPU mémorisé (ne pas retenter)
	int64_t          tick      = 0;            // pts monotone (time_base 1/1000)
	int              builds    = 0;            // nb de (re)constructions du graphe (diagnostic, jamais remis à zéro)
};

#endif
