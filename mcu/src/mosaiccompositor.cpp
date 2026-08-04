#include "log.h"
#include "mosaiccompositor.h"
#include <string>
#include <string.h>
extern "C"
{
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
}

// Options communes à tous les filtres 'overlay' (framesync) : chaque tick pousse
// TOUTES les entrées au même pts, donc framesync a toujours une paire exacte et
// le sink rend exactement une trame (cf. mosaic_avfilter_plan.md §4).
#define SYNC_OPTS "eof_action=pass:repeatlast=1:shortest=0"

MosaicCompositor::MosaicCompositor()
{
}

MosaicCompositor::~MosaicCompositor()
{
	Release();
}

void MosaicCompositor::Release()
{
	if (graph)
		avfilter_graph_free(&graph);   // libère TOUS les AVFilterContext du graphe
	graph        = nullptr;
	sinkCtx      = nullptr;
	bgSrc        = nullptr;
	mosaicOvrSrc = nullptr;
	slotSrcs.clear();
	ovrSrcs.clear();
	gpuBackground.reset();
	tick         = 0;
	// cur et gpuBroken sont volontairement conservés (mémo de reconfig / échec GPU).
}

// Deux vignettes (emprise liseré comprise) se chevauchent-elles ? Vrai pour les
// dispositions PIP (incrustations posées SUR l'image principale plein cadre).
bool MosaicCompositor::SlotsOverlap() const
{
	for (size_t i = 0; i < cur.slots.size(); i++)
		for (size_t j = i + 1; j < cur.slots.size(); j++)
		{
			const MosaicSlotDesc& a = cur.slots[i];
			const MosaicSlotDesc& b = cur.slots[j];
			const int ax0 = a.x - a.border, ay0 = a.y - a.border;
			const int ax1 = a.x + a.w + a.border, ay1 = a.y + a.h + a.border;
			const int bx0 = b.x - b.border, by0 = b.y - b.border;
			const int bx1 = b.x + b.w + b.border, by1 = b.y + b.h + b.border;
			if (ax0 < bx1 && bx0 < ax1 && ay0 < by1 && by0 < ay1)
				return true;
		}
	return false;
}

// Fond du mode GPU : la couleur de base, plus un rectangle NOIR sous l'emprise
// (liseré compris) de chaque vignette. En mode CPU le liseré vient d'un pad par
// image ; sur GPU (pas de pad_vaapi) il vient du fond, qui reste visible sur
// 'border' px autour de chaque image posée par-dessus. Y=16 : le même noir
// vidéo (limited range) que produit le pad CPU. U/V restent neutres (128) dans
// les deux couleurs de base, seul le plan Y est peint.
bool MosaicCompositor::BuildGpuBackground()
{
	gpuBackground = cur.blackBackground
	              ? Pict::CreateBlack(cur.width, cur.height)
	              : Pict::CreateColor(cur.width, cur.height, 128, 128, 128);
	if (!gpuBackground || !gpuBackground->GetAVFrame())
		return false;

	AVFrame* f = gpuBackground->GetAVFrame();   // frais de fabrique : seul détenteur
	for (const MosaicSlotDesc& s : cur.slots)
	{
		int x0 = s.x - s.border, y0 = s.y - s.border;
		int x1 = s.x + s.w + s.border, y1 = s.y + s.h + s.border;
		if (x0 < 0) x0 = 0;
		if (y0 < 0) y0 = 0;
		if (x1 > cur.width)  x1 = cur.width;
		if (y1 > cur.height) y1 = cur.height;
		for (int y = y0; y < y1; y++)
			memset(f->data[0] + y * f->linesize[0] + x0, 16, x1 - x0);
	}
	return true;
}

bool MosaicCompositor::Configure(const MosaicGraphDesc& desc)
{
	// Réutilise le graphe existant si la description n'a pas changé.
	if (graph && desc == cur)
		return true;

	Release();
	cur = desc;

	// Politique GPU/CPU (§2) : GPU d'abord si souhaité et pas déjà cassé, repli CPU.
	if (cur.wantGPU && !gpuBroken)
	{
		if (BuildGraph(true))
		{
			curGPU = true;
			return true;
		}
		Error("-MosaicCompositor: echec construction graphe GPU, repli CPU\n");
		gpuBroken = true;
		Release();      // libère un graphe GPU partiel (ne touche pas cur/gpuBroken)
	}

	if (BuildGraph(false))
	{
		curGPU = false;
		return true;
	}

	Release();
	return false;
}

bool MosaicCompositor::BuildGraph(bool gpu)
{
	if (cur.width <= 0 || cur.height <= 0)
		return false;

	// Overlays présents ? En mode GPU ils imposent une queue CPU (overlay_vaapi
	// ne gère pas l'alpha RGBA de façon fiable, cf. plan §2.5).
	bool anyOverlay = cur.hasMosaicOverlay;
	for (const MosaicSlotDesc& s : cur.slots)
		if (s.hasOverlay)
			anyOverlay = true;

	AVBufferRef* device = nullptr;
	if (gpu)
	{
		device = Pict::GetVAAPIDevice();
		if (!device)
			return false;   // pas d'accélération sur cette machine
		if (cur.slots.empty())
			return false;   // rien à composer : autant servir le fond en CPU
		if (SlotsOverlap())
		{
			// PIP : le liseré d'une incrustation devrait passer PAR-DESSUS
			// l'image principale, or sur GPU il est peint dans le fond (pas de
			// pad_vaapi). Repli CPU assumé pour ces dispositions.
			Log("-MosaicCompositor: vignettes superposees (PIP), repli CPU\n");
			return false;
		}
		if (!BuildGpuBackground())
			return false;
	}

	graph = avfilter_graph_alloc();
	if (!graph)
		return false;

	char args[256];
	char name[32];

	// --- buffersrc du fond (WxH, yuv420p — uploadé par le graphe en GPU) ----
	snprintf(args, sizeof(args),
	         "video_size=%dx%d:pix_fmt=%d:time_base=1/1000:pixel_aspect=1/1",
	         cur.width, cur.height, AV_PIX_FMT_YUV420P);
	if (avfilter_graph_create_filter(&bgSrc, avfilter_get_by_name("buffer"),
	                                 "bg", args, NULL, graph) < 0)
		return false;

	// --- buffersrc des slots actifs (+ overlay participant éventuel) --------
	slotSrcs.assign(cur.slots.size(), nullptr);
	ovrSrcs.assign(cur.slots.size(), nullptr);
	for (size_t i = 0; i < cur.slots.size(); i++)
	{
		const MosaicSlotDesc& s = cur.slots[i];
		snprintf(args, sizeof(args),
		         "video_size=%dx%d:pix_fmt=%d:time_base=1/1000:pixel_aspect=1/1",
		         s.inW, s.inH, s.inFmt);
		snprintf(name, sizeof(name), "in%zu", i);
		if (avfilter_graph_create_filter(&slotSrcs[i], avfilter_get_by_name("buffer"),
		                                 name, args, NULL, graph) < 0)
			return false;

		// Entrée GPU : le buffersrc doit connaître le hw_frames_ctx de la
		// surface (même patron que VideoRescaler) pour que scale_vaapi négocie.
		if (s.hwFramesCtx)
		{
			AVBufferSrcParameters* par = av_buffersrc_parameters_alloc();
			if (!par)
				return false;
			par->format        = s.inFmt;
			par->width         = s.inW;
			par->height        = s.inH;
			par->time_base     = av_make_q(1, 1000);
			par->hw_frames_ctx = s.hwFramesCtx;
			int ret = av_buffersrc_parameters_set(slotSrcs[i], par);
			av_free(par);
			if (ret < 0)
				return false;
		}

		if (s.hasOverlay)
		{
			snprintf(args, sizeof(args),
			         "video_size=%dx%d:pix_fmt=%d:time_base=1/1000:pixel_aspect=1/1",
			         s.ovW, s.ovH, AV_PIX_FMT_RGBA);
			snprintf(name, sizeof(name), "ovr%zu", i);
			if (avfilter_graph_create_filter(&ovrSrcs[i], avfilter_get_by_name("buffer"),
			                                 name, args, NULL, graph) < 0)
				return false;
		}
	}

	// --- buffersrc de l'overlay mosaïque plein cadre (rgba WxH) -------------
	if (cur.hasMosaicOverlay)
	{
		snprintf(args, sizeof(args),
		         "video_size=%dx%d:pix_fmt=%d:time_base=1/1000:pixel_aspect=1/1",
		         cur.width, cur.height, AV_PIX_FMT_RGBA);
		if (avfilter_graph_create_filter(&mosaicOvrSrc, avfilter_get_by_name("buffer"),
		                                 "movr", args, NULL, graph) < 0)
			return false;
	}

	// --- buffersink : yuv420p (CPU, ou GPU avec queue overlays), sinon VAAPI -
	if (avfilter_graph_create_filter(&sinkCtx, avfilter_get_by_name("buffersink"),
	                                 "out", NULL, NULL, graph) < 0)
		return false;
	const enum AVPixelFormat outFmt =
		(gpu && !anyOverlay) ? AV_PIX_FMT_VAAPI : AV_PIX_FMT_YUV420P;
	const enum AVPixelFormat pix_fmts[] = { outFmt, AV_PIX_FMT_NONE };
	if (av_opt_set_int_list(sinkCtx, "pix_fmts", pix_fmts, AV_PIX_FMT_NONE,
	                        AV_OPT_SEARCH_CHILDREN) < 0)
		return false;

	// --- description du graphe (chaînes scale + cascade overlay) -----------
	std::string desc;
	char frag[256];

	// Chaîne d'entrée par slot.
	// CPU : [inK] [hwdownload,]scale=w:h,format=yuv420p[,pad liseré noir] [sK]
	//   — le pad ajoute 'border' px de noir autour de l'image, garanti noir quel
	//   que soit le fond ; une entrée GPU en mode CPU est redescendue DANS la
	//   branche (une traversée, pas de DownloadToCPU hors graphe, plan §2.3).
	// GPU : [inK] [format=nv12,hwupload,]scale_vaapi=w:h [sK]
	//   — pas de pad_vaapi : le liseré est peint dans le fond (BuildGpuBackground).
	for (size_t i = 0; i < cur.slots.size(); i++)
	{
		const MosaicSlotDesc& s = cur.slots[i];
		if (gpu)
			snprintf(frag, sizeof(frag),
			         "[in%zu] %sscale_vaapi=w=%d:h=%d [s%zu];",
			         i, s.hwFramesCtx ? "" : "format=nv12,hwupload,",
			         s.w, s.h, i);
		else if (s.border > 0)
			snprintf(frag, sizeof(frag),
			         "[in%zu] %sscale=%d:%d,format=yuv420p,"
			         "pad=%d:%d:%d:%d:black [s%zu];",
			         i, s.hwFramesCtx ? "hwdownload,format=nv12," : "",
			         s.w, s.h,
			         s.w + 2*s.border, s.h + 2*s.border,
			         s.border, s.border, i);
		else
			snprintf(frag, sizeof(frag),
			         "[in%zu] %sscale=%d:%d,format=yuv420p [s%zu];",
			         i, s.hwFramesCtx ? "hwdownload,format=nv12," : "",
			         s.w, s.h, i);
		desc += frag;
	}

	// Fond : en mode GPU il monte en VRAM (nv12, format de surface nominal des
	// pools VAAPI) une fois par tick.
	std::string prev = "bg";
	if (gpu)
	{
		desc += "[bg] format=nv12,hwupload [bgup];";
		prev = "bgup";
	}

	// cascade overlay des vignettes : [prev][sK] overlay(_vaapi)=x:y [cK]
	// CPU : (x,y) désignent l'IMAGE, la vignette paddée est posée décalée du
	// liseré. GPU : l'image nue est posée en (x,y), le liseré peint dans le
	// fond reste visible autour.
	for (size_t i = 0; i < cur.slots.size(); i++)
	{
		const MosaicSlotDesc& s = cur.slots[i];
		std::string next = "c" + std::to_string(i);
		if (gpu)
			snprintf(frag, sizeof(frag),
			         "[%s][s%zu] overlay_vaapi=x=%d:y=%d:" SYNC_OPTS " [%s];",
			         prev.c_str(), i, s.x, s.y, next.c_str());
		else
			snprintf(frag, sizeof(frag),
			         "[%s][s%zu] overlay=x=%d:y=%d:" SYNC_OPTS " [%s];",
			         prev.c_str(), i, s.x - s.border, s.y - s.border, next.c_str());
		desc += frag;
		prev = next;
	}

	// Queue CPU du mode GPU (plan §2.5) : les overlays RGBA se composent en CPU,
	// le composite redescend UNE fois, après le gros du travail (scale+compo GPU).
	if (gpu && anyOverlay)
	{
		snprintf(frag, sizeof(frag),
		         "[%s] hwdownload,format=nv12,format=yuv420p [cpuq];", prev.c_str());
		desc += frag;
		prev = "cpuq";
	}

	// overlays participants (empilés APRÈS toutes les vignettes) :
	// [prev][ovrK] overlay=ovX:ovY [pK]
	for (size_t i = 0; i < cur.slots.size(); i++)
	{
		if (!cur.slots[i].hasOverlay)
			continue;
		const MosaicSlotDesc& s = cur.slots[i];
		std::string next = "p" + std::to_string(i);
		snprintf(frag, sizeof(frag),
		         "[%s][ovr%zu] overlay=x=%d:y=%d:" SYNC_OPTS " [%s];",
		         prev.c_str(), i, s.ovX, s.ovY, next.c_str());
		desc += frag;
		prev = next;
	}

	// overlay mosaïque plein cadre (par-dessus tout) : [prev][movr] overlay=0:0 [m]
	if (cur.hasMosaicOverlay)
	{
		snprintf(frag, sizeof(frag),
		         "[%s][movr] overlay=x=0:y=0:" SYNC_OPTS " [movout];", prev.c_str());
		desc += frag;
		prev = "movout";
	}

	// Cas dégénéré : aucun slot, aucun overlay → simple passe du fond.
	if (prev == "bg")
	{
		desc += "[bg] null [nullout];";
		prev = "nullout";
	}

	// Retire le ';' final (parse_ptr n'aime pas un chaînon vide en fin).
	if (!desc.empty() && desc.back() == ';')
		desc.pop_back();

	// --- pads ouverts : sorties = buffersrc, entrée = buffersink ------------
	AVFilterInOut* outputs = NULL;   // pads de sortie ouverts (nos buffersrc)
	auto addOut = [&](const char* nm, AVFilterContext* ctx) -> bool
	{
		AVFilterInOut* io = avfilter_inout_alloc();
		if (!io)
			return false;
		io->name       = av_strdup(nm);
		io->filter_ctx = ctx;
		io->pad_idx    = 0;
		io->next       = outputs;
		outputs        = io;
		return true;
	};

	bool ok = addOut("bg", bgSrc);
	for (size_t i = 0; ok && i < cur.slots.size(); i++)
	{
		snprintf(name, sizeof(name), "in%zu", i);
		ok = addOut(name, slotSrcs[i]);
		if (ok && cur.slots[i].hasOverlay)
		{
			snprintf(name, sizeof(name), "ovr%zu", i);
			ok = addOut(name, ovrSrcs[i]);
		}
	}
	if (ok && cur.hasMosaicOverlay)
		ok = addOut("movr", mosaicOvrSrc);

	AVFilterInOut* inputs = avfilter_inout_alloc();  // pad d'entrée ouvert (le sink)
	if (ok && inputs)
	{
		inputs->name       = av_strdup(prev.c_str());
		inputs->filter_ctx = sinkCtx;
		inputs->pad_idx    = 0;
		inputs->next       = NULL;
	}
	else
	{
		ok = false;
	}

	int ret = -1;
	if (ok)
	{
		ret = avfilter_graph_parse_ptr(graph, desc.c_str(), &inputs, &outputs, NULL);
		if (ret >= 0 && gpu)
		{
			// Équivalent du -filter_hw_device de ffmpeg : les hwupload (et tout
			// filtre créé par le parse qui en aurait besoin) reçoivent le device
			// VAAPI partagé AVANT la config, faute de quoi la négociation échoue.
			for (unsigned i = 0; i < graph->nb_filters; i++)
				if (!graph->filters[i]->hw_device_ctx)
					graph->filters[i]->hw_device_ctx = av_buffer_ref(device);
		}
		if (ret >= 0)
			ret = avfilter_graph_config(graph, NULL);
		ok = (ret >= 0);
	}
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);

	if (!ok)
	{
		Error("-MosaicCompositor: graph config failed (%d) [%s]\n", ret, desc.c_str());
		return false;   // Configure fera le Release
	}

	// Trace de (re)construction. Une reconstruction est NORMALE au changement de
	// topologie (slots, type de mosaïque, VAD) ou de géométrie d'entrée ; en
	// rafale (compteur qui grimpe à chaque tick) elle signale une résolution
	// d'entrée qui oscille — la clé de reconfig inclut inW/inH/inFmt par slot.
	builds++;
	std::string slotsInfo;
	for (size_t i = 0; i < cur.slots.size(); i++)
	{
		const MosaicSlotDesc& s = cur.slots[i];
		snprintf(frag, sizeof(frag), "%spos%d %dx%d(fmt%d)->%dx%d@%d,%d(b%d)%s",
		         i ? " " : "", s.pos, s.inW, s.inH, s.inFmt,
		         s.w, s.h, s.x, s.y, s.border, s.hasOverlay ? "+ovr" : "");
		slotsInfo += frag;
	}
	Log("-MosaicCompositor: graphe #%d construit [%s %dx%d, fond %s, %d slot(s): %s%s] desc=[%s]\n",
	    builds, gpu ? "GPU" : "CPU", cur.width, cur.height,
	    cur.blackBackground ? "noir" : "gris",
	    (int)cur.slots.size(), slotsInfo.c_str(),
	    cur.hasMosaicOverlay ? ", overlay mosaique" : "", desc.c_str());
	return true;
}

bool MosaicCompositor::PushInput(AVFilterContext* src, const PictPtr& pic, int64_t pts)
{
	if (!src || !pic || !pic->GetAVFrame())
		return false;

	// Conteneur éphémère : on pose le pts sur une RÉFÉRENCE, jamais sur l'AVFrame
	// partagé du Pict (cf. §6). av_buffersrc_add_frame prend la référence (t vidé).
	AVFrame* t = av_frame_alloc();
	if (!t)
		return false;
	if (av_frame_ref(t, pic->GetAVFrame()) < 0)
	{
		av_frame_free(&t);
		return false;
	}
	t->pts = pts;
	int ret = av_buffersrc_add_frame(src, t);
	av_frame_free(&t);
	if (ret < 0)
	{
		Error("-MosaicCompositor: buffersrc_add_frame failed (%d)\n", ret);
		return false;
	}
	return true;
}

PictPtr MosaicCompositor::Compose(const std::vector<PictPtr>& slotFrames,
                                  const std::vector<PictPtr>& slotOverlays,
                                  const PictPtr& background,
                                  const PictPtr& mosaicOverlay)
{
	if (!graph || slotFrames.size() != cur.slots.size())
		return nullptr;

	// Tous les push d'un tick partagent le même pts monotone (framesync, §4).
	const int64_t pts = tick++;

	// En mode GPU le fond de l'appelant est remplacé par le fond du compositor
	// (liseré noir peint sous chaque vignette, cf. BuildGpuBackground).
	const PictPtr& bg = (curGPU && gpuBackground) ? gpuBackground : background;
	if (!PushInput(bgSrc, bg, pts))
		return nullptr;

	for (size_t i = 0; i < cur.slots.size(); i++)
	{
		if (!PushInput(slotSrcs[i], slotFrames[i], pts))
			return nullptr;
		if (cur.slots[i].hasOverlay)
		{
			const PictPtr& ov = (i < slotOverlays.size()) ? slotOverlays[i] : nullptr;
			if (!PushInput(ovrSrcs[i], ov, pts))
				return nullptr;
		}
	}

	if (cur.hasMosaicOverlay && !PushInput(mosaicOvrSrc, mosaicOverlay, pts))
		return nullptr;

	AVFrame* out = av_frame_alloc();
	if (!out)
		return nullptr;
	int ret = av_buffersink_get_frame(sinkCtx, out);
	if (ret < 0)
	{
		if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
			Error("-MosaicCompositor: buffersink_get_frame failed (%d)\n", ret);
		av_frame_free(&out);
		return nullptr;
	}

	return std::make_shared<Pict>(out);
}
