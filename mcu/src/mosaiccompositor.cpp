#include "log.h"
#include "mosaiccompositor.h"
#include <string>
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
	tick         = 0;
	// cur et gpuBroken sont volontairement conservés (mémo de reconfig / échec GPU).
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
	if (gpu)
		return false;   // chemin VAAPI : Phase 5

	if (cur.width <= 0 || cur.height <= 0)
		return false;

	graph = avfilter_graph_alloc();
	if (!graph)
		return false;

	char args[256];
	char name[32];

	// --- buffersrc du fond (WxH, yuv420p) -----------------------------------
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

	// --- buffersink contraint en yuv420p (mode CPU) ------------------------
	if (avfilter_graph_create_filter(&sinkCtx, avfilter_get_by_name("buffersink"),
	                                 "out", NULL, NULL, graph) < 0)
		return false;
	const enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE };
	if (av_opt_set_int_list(sinkCtx, "pix_fmts", pix_fmts, AV_PIX_FMT_NONE,
	                        AV_OPT_SEARCH_CHILDREN) < 0)
		return false;

	// --- description du graphe (chaînes scale + cascade overlay) -----------
	std::string desc;
	char frag[256];

	// scale par slot : [inK] scale=w:h,format=yuv420p[,pad liseré noir] [sK].
	// Le pad ajoute 'border' px de noir autour de l'IMAGE mise à l'échelle : le
	// liseré est garanti noir quel que soit le fond de la mosaïque (gris/noir).
	for (size_t i = 0; i < cur.slots.size(); i++)
	{
		const MosaicSlotDesc& s = cur.slots[i];
		if (s.border > 0)
			snprintf(frag, sizeof(frag),
			         "[in%zu] scale=%d:%d,format=yuv420p,"
			         "pad=%d:%d:%d:%d:black [s%zu];",
			         i, s.w, s.h,
			         s.w + 2*s.border, s.h + 2*s.border,
			         s.border, s.border, i);
		else
			snprintf(frag, sizeof(frag),
			         "[in%zu] scale=%d:%d,format=yuv420p [s%zu];", i, s.w, s.h, i);
		desc += frag;
	}

	// cascade overlay des vignettes : [prev][sK] overlay=x:y [cK]
	// (x,y) désignent l'IMAGE ; la vignette paddée est donc posée liseré compris,
	// décalée du liseré vers le haut/gauche.
	std::string prev = "bg";
	for (size_t i = 0; i < cur.slots.size(); i++)
	{
		const MosaicSlotDesc& s = cur.slots[i];
		std::string next = "c" + std::to_string(i);
		snprintf(frag, sizeof(frag),
		         "[%s][s%zu] overlay=x=%d:y=%d:" SYNC_OPTS " [%s];",
		         prev.c_str(), i, s.x - s.border, s.y - s.border, next.c_str());
		desc += frag;
		prev = next;
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

	if (!PushInput(bgSrc, background, pts))
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
