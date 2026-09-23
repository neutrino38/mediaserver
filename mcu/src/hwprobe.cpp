#include <chrono>
#include <functional>
#include <memory>
#include <deque>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include "log.h"
#include "hwprobe.h"
#include "mosaiccompositor.h"
#include "h264/h264encoder.h"
#include "h264/h264decoder.h"
#include "vp8/vp8encoder.h"
#include "medkit/videorescaler.h"
extern "C"
{
#include <libavcodec/avcodec.h>
}

namespace {

const int W = 320, H = 240;
const int Frames = 20;
const BYTE BandLuma[4] = { 40, 90, 140, 190 };
const BYTE MireU = 90, MireV = 160;

struct Outcome
{
	HwProbeState state;
	std::string  detail;
};

Outcome Ok(const std::string& d)     { return { HwProbeState::Ok, d }; }
Outcome Absent(const std::string& d) { return { HwProbeState::Absent, d }; }
Outcome Failed(const std::string& d) { return { HwProbeState::Failed, d }; }

// Quatre bandes verticales de luma connue, chroma non neutre : une chroma
// fausse sur une luma juste trahit un mauvais format de surface.
PictPtr CreateMire()
{
	PictPtr pic = Pict::CreateColor(W, H, BandLuma[0], MireU, MireV);
	if (!pic)
		return nullptr;
	AVFrame* f = pic->GetAVFrame();
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			f->data[0][y * f->linesize[0] + x] = BandLuma[x * 4 / W];
	return pic;
}

// Vérifie la mire reportée dans le rectangle (x0,y0,w,h) de `pic` (YUV420P).
// Rend une chaîne vide si elle est juste, sinon le premier écart.
std::string CheckMire(const PictPtr& pic, int x0, int y0, int w, int h, int tol)
{
	if (!pic || !pic->GetAVFrame())
		return "pas d'image";
	AVFrame* f = pic->GetAVFrame();
	if (f->format != AV_PIX_FMT_YUV420P)
		return "format " + std::to_string(f->format);
	int y = y0 + h / 2;
	for (int b = 0; b < 4; b++)
	{
		int x = x0 + (2 * b + 1) * w / 8;
		int Y = f->data[0][y * f->linesize[0] + x];
		int U = f->data[1][(y / 2) * f->linesize[1] + x / 2];
		int V = f->data[2][(y / 2) * f->linesize[2] + x / 2];
		if (abs(Y - BandLuma[b]) > tol || abs(U - MireU) > tol || abs(V - MireV) > tol)
			return "bande " + std::to_string(b) + " Y=" + std::to_string(Y) + " U=" + std::to_string(U)
			     + " V=" + std::to_string(V) + " attendu " + std::to_string(BandLuma[b]) + "/"
			     + std::to_string(MireU) + "/" + std::to_string(MireV);
	}
	return "";
}

PictPtr ToCpu(const PictPtr& pic)
{
	return pic && pic->IsGPUPict() ? pic->DownloadToCPU() : pic;
}

// H264Encoder rend de l'AVCC ; les décodeurs veulent de l'Annex-B. spsFlags >= 0
// réécrit l'octet de contraintes du SPS : c'est ce que déclare un terminal tiers.
std::vector<BYTE> ToAnnexB(const VideoFramePtr& vf, int spsFlags = -1)
{
	std::vector<BYTE> o;
	BYTE* d = vf->GetData();
	DWORD len = vf->GetLength(), p = 0;
	while (p + 4 <= len)
	{
		DWORD n = (d[p] << 24) | (d[p+1] << 16) | (d[p+2] << 8) | d[p+3];
		if (p + 4 + n > len)
			break;
		size_t at = o.size() + 4;
		o.insert(o.end(), { 0, 0, 0, 1 });
		o.insert(o.end(), d + p + 4, d + p + 4 + n);
		if (spsFlags >= 0 && n >= 4 && (o[at] & 0x1f) == 7)
			o[at + 2] = (BYTE)spsFlags;
		p += 4 + n;
	}
	return o;
}

// Rend l'image produite par ce paquet, nullptr si le décodeur n'en a pas rendu.
PictPtr DecodeOne(VideoDecoder& dec, std::vector<BYTE> data)
{
	if (data.empty())
		return nullptr;
	size_t len = data.size();
	data.resize(len + AV_INPUT_BUFFER_PADDING_SIZE, 0);
	PictPtr before = dec.GetFrame();
	dec.Decode(data.data(), len);
	PictPtr pic = dec.GetFrame();
	return pic != before ? pic : nullptr;
}

// Décodeur libavcodec sans device : juge un encodeur matériel sans dépendre du
// décodage matériel.
class SoftwareDecoder
{
public:
	explicit SoftwareDecoder(AVCodecID id)
	{
		const AVCodec* codec = avcodec_find_decoder(id);
		ctx = codec ? avcodec_alloc_context3(codec) : nullptr;
		if (ctx && avcodec_open2(ctx, codec, nullptr) < 0)
			avcodec_free_context(&ctx);
	}
	~SoftwareDecoder() { avcodec_free_context(&ctx); }

	PictPtr Decode(std::vector<BYTE> data)
	{
		if (!ctx || data.empty())
			return nullptr;
		size_t len = data.size();
		data.resize(len + AV_INPUT_BUFFER_PADDING_SIZE, 0);
		AVPacket* pkt = av_packet_alloc();
		pkt->data = data.data();
		pkt->size = len;
		PictPtr last;
		if (avcodec_send_packet(ctx, pkt) >= 0)
			for (;;)
			{
				AVFrame* f = av_frame_alloc();
				if (avcodec_receive_frame(ctx, f) < 0)
				{
					av_frame_free(&f);
					break;
				}
				last = std::make_shared<Pict>(f);
			}
		av_packet_free(&pkt);
		return last;
	}

private:
	AVCodecContext* ctx = nullptr;
};

std::unique_ptr<H264Encoder> OpenH264Encoder(bool hardware, const char* plid)
{
	Properties props;
	if (hardware)
		props.SetProperty("video.hwaccel.required", "1");
	else
		props.SetProperty("video.hwaccel", "0");
	if (plid)
		props.SetProperty("h264.profile-level-id", plid);
	std::unique_ptr<H264Encoder> enc(new H264Encoder(props));
	enc->SetFrameRate(25, 512, 50);
	if (enc->SetSize(W, H) < 1)
		return nullptr;
	return enc;
}

Outcome ProbeUpload(PictPtr& gpuMire)
{
	PictPtr mire = CreateMire();
	int ret = mire ? mire->UploadToGPU(gpuMire) : -1;
	if (ret < 0 || !gpuMire || !gpuMire->IsGPUPict())
		return Failed("UploadToGPU a rendu " + std::to_string(ret));
	std::string err = CheckMire(gpuMire->DownloadToCPU(), 0, 0, W, H, 2);
	return err.empty() ? Ok("mire juste apres aller-retour") : Failed(err);
}

Outcome ProbeDownload(const PictPtr& gpuMire)
{
	if (!gpuMire)
		return Failed("pas de surface a redescendre");
	std::string err = CheckMire(gpuMire->DownloadToCPU(), 0, 0, W, H, 2);
	return err.empty() ? Ok("luma et chroma a +-2") : Failed(err);
}

Outcome ProbeH264Encode()
{
	std::unique_ptr<H264Encoder> enc = OpenH264Encoder(true, nullptr);
	if (!enc || !enc->IsHardwareReady())
		return Failed("encodeur h264_vaapi non ouvert");
	SoftwareDecoder dec(AV_CODEC_ID_H264);
	PictPtr mire = CreateMire();
	int packets = 0;
	PictPtr last;
	for (int i = 0; i < Frames; i++)
	{
		VideoFramePtr vf = enc->EncodeFrame(mire);
		if (!vf)
			continue;
		packets++;
		if (PictPtr pic = dec.Decode(ToAnnexB(vf)))
			last = pic;
	}
	std::string counts = std::to_string(packets) + "/" + std::to_string(Frames) + " paquets";
	if (packets < Frames - 4)
		return Failed(counts);
	std::string err = CheckMire(last, 0, 0, W, H, 8);
	return err.empty() ? Ok(counts + ", mire juste") : Failed(counts + ", " + err);
}

// spsFlags : octet de contraintes déclaré au décodeur, -1 pour garder celui
// de l'encodeur.
Outcome ProbeH264Decode(const char* plid, int spsFlags)
{
	std::unique_ptr<H264Encoder> enc = OpenH264Encoder(false, plid);
	if (!enc)
		return Failed("encodeur logiciel de reference non ouvert");
	H264Decoder dec(/*requireHW*/ true);
	if (!dec.IsHardwareReady())
		return Failed("decodeur VAAPI non ouvert");
	PictPtr mire = CreateMire();
	int packets = 0, gpu = 0;
	PictPtr last;
	for (int i = 0; i < Frames; i++)
	{
		VideoFramePtr vf = enc->EncodeFrame(mire);
		if (!vf)
			continue;
		packets++;
		PictPtr pic = DecodeOne(dec, ToAnnexB(vf, spsFlags));
		if (pic && pic->IsGPUPict())
		{
			gpu++;
			last = pic;
		}
	}
	std::string counts = std::to_string(gpu) + "/" + std::to_string(packets) + " images sur GPU";
	if (packets == 0 || gpu < packets - 1)
		return Failed(counts);
	std::string err = CheckMire(ToCpu(last), 0, 0, W, H, 8);
	return err.empty() ? Ok(counts + ", mire juste") : Failed(counts + ", " + err);
}

Outcome ProbeVp8Decode()
{
	Properties props;
	VP8Encoder enc(props);
	enc.SetFrameRate(25, 512, 50);
	if (enc.SetSize(W, H) < 1)
		return Failed("encodeur VP8 logiciel de reference non ouvert");
	FfVideoDecoder dec(AV_CODEC_ID_VP8, VideoCodec::VP8, nullptr, /*requireHW*/ true);
	if (!dec.IsHardwareReady())
		return Failed("decodeur VAAPI non ouvert");
	PictPtr mire = CreateMire();
	int packets = 0, gpu = 0;
	PictPtr last;
	for (int i = 0; i < Frames; i++)
	{
		VideoFramePtr vf = enc.EncodeFrame(mire);
		if (!vf)
			continue;
		packets++;
		PictPtr pic = DecodeOne(dec, std::vector<BYTE>(vf->GetData(), vf->GetData() + vf->GetLength()));
		if (pic && pic->IsGPUPict())
		{
			gpu++;
			last = pic;
		}
	}
	std::string counts = std::to_string(gpu) + "/" + std::to_string(packets) + " images sur GPU";
	if (packets == 0 || gpu < packets - 1)
		return Failed(counts);
	std::string err = CheckMire(ToCpu(last), 0, 0, W, H, 8);
	return err.empty() ? Ok(counts + ", mire juste") : Failed(counts + ", " + err);
}

Outcome ProbeScale(const PictPtr& gpuMire)
{
	if (!gpuMire)
		return Failed("pas de surface source");
	const int OW = W / 2, OH = H / 2;
	VideoRescaler rescaler;
	PictPtr out = rescaler.Rescale(gpuMire, OW, OH, false);
	if (!out)
		return Failed("Rescale a rendu nullptr");
	if (!out->IsGPUPict())
		return Failed("sortie hors GPU");
	std::string err = CheckMire(out->DownloadToCPU(), 0, 0, OW, OH, 4);
	return err.empty() ? Ok(std::to_string(OW) + "x" + std::to_string(OH) + ", mire juste") : Failed(err);
}

// Deux slots côte à côte : la mire en surface GPU, une couleur unie en CPU. Le
// fond GPU porte le liseré noir de chaque slot.
Outcome ProbeMosaic(const PictPtr& gpuMire)
{
	if (!gpuMire)
		return Failed("pas de surface source");
	const BYTE CpuLuma = 120, BgLuma = 128, BorderLuma = 16;

	MosaicGraphDesc d;
	d.width = 640; d.height = 360; d.wantGPU = true;
	MosaicSlotDesc gpuSlot;
	gpuSlot.pos = 0; gpuSlot.x = 2; gpuSlot.y = 2; gpuSlot.w = 316; gpuSlot.h = 176; gpuSlot.border = 2;
	gpuSlot.inW = W; gpuSlot.inH = H; gpuSlot.inFmt = AV_PIX_FMT_VAAPI;
	gpuSlot.hwFramesCtx = gpuMire->GetAVFrame()->hw_frames_ctx;
	gpuSlot.hwFrames = gpuSlot.hwFramesCtx->data;
	MosaicSlotDesc cpuSlot = gpuSlot;
	cpuSlot.pos = 1; cpuSlot.x = 322;
	cpuSlot.inFmt = AV_PIX_FMT_YUV420P; cpuSlot.hwFramesCtx = nullptr; cpuSlot.hwFrames = nullptr;
	d.slots = { gpuSlot, cpuSlot };

	MosaicCompositor comp;
	if (!comp.Configure(d))
		return Failed("Configure a echoue");
	PictPtr out = comp.Compose({ gpuMire, Pict::CreateColor(W, H, CpuLuma, 128, 128) },
	                           std::vector<PictPtr>(),
	                           Pict::CreateColor(d.width, d.height, BgLuma, 128, 128), nullptr);
	if (!out)
		return Failed("Compose a rendu nullptr");
	if (!out->IsGPUPict())
		return Failed("composition retombee sur le CPU");
	PictPtr cpu = out->DownloadToCPU();
	std::string err = CheckMire(cpu, gpuSlot.x, gpuSlot.y, gpuSlot.w, gpuSlot.h, 4);
	if (!err.empty())
		return Failed("slot GPU : " + err);
	AVFrame* f = cpu->GetAVFrame();
	auto luma = [f](int x, int y) { return (int)f->data[0][y * f->linesize[0] + x]; };
	int cpuY = luma(cpuSlot.x + cpuSlot.w / 2, cpuSlot.y + cpuSlot.h / 2);
	int borderY = luma(gpuSlot.x - 1, gpuSlot.y + gpuSlot.h / 2);
	int bgY = luma(d.width / 2, d.height - 20);
	if (abs(cpuY - CpuLuma) > 4)
		return Failed("slot CPU Y=" + std::to_string(cpuY));
	if (abs(borderY - BorderLuma) > 4)
		return Failed("lisere Y=" + std::to_string(borderY));
	if (abs(bgY - BgLuma) > 4)
		return Failed("fond Y=" + std::to_string(bgY));
	return Ok("slot GPU, slot CPU, lisere et fond justes");
}

// 42801F : SPS rabattu sur 0x80, ce que déclare un terminal SIP (l'encodeur,
// lui, pose constraint_set1).
const struct { const char* plid; int spsFlags; } Profiles[] = {
	{ "42e01f", -1 }, { "42801F", 0x80 }, { "4d001f", -1 }, { "64001f", -1 },
};

// Simule une sonde tuée ou bloquée par le driver (cf. hwprobe.h).
void InjectFault(const std::string& capability)
{
	const char* fault = getenv("MCU_HWPROBE_FAULT");
	if (!fault)
		return;
	std::string f(fault);
	if (f == "abort:" + capability)
	{
		rlimit noCore = { 0, 0 };
		setrlimit(RLIMIT_CORE, &noCore);
		abort();
	}
	if (f == "hang:" + capability)
		for (;;)
			pause();
}

HwProbeState ParseState(const std::string& s, bool& known)
{
	known = true;
	if (s == "ok")     return HwProbeState::Ok;
	if (s == "absent") return HwProbeState::Absent;
	known = (s == "echec");
	return HwProbeState::Failed;
}

} // namespace

const char* HwProbeStateName(HwProbeState state)
{
	switch (state)
	{
		case HwProbeState::Ok:     return "ok";
		case HwProbeState::Absent: return "absent";
		default:                   return "echec";
	}
}

const std::vector<std::string>& HwProbeCapabilities()
{
	static const std::vector<std::string> caps = [] {
		std::vector<std::string> c = { "device", "upload", "download", "h264.encode" };
		for (const auto& p : Profiles)
			c.push_back(std::string("h264.decode.") + p.plid);
		for (const char* n : { "h264.decode", "vp8.decode", "vp8.encode", "av1.decode", "scale", "mosaic" })
			c.push_back(n);
		return c;
	}();
	return caps;
}

std::vector<HwProbeVerdict> RunHwProbe(FILE* out)
{
	std::vector<HwProbeVerdict> verdicts;
	bool device = Pict::GetVAAPIDevice() != nullptr;

	auto judge = [&](const std::string& capability, const std::function<Outcome()>& probe)
	{
		InjectFault(capability);
		auto start = std::chrono::steady_clock::now();
		Outcome o = device ? probe() : Absent("pas de device VAAPI");
		long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
		verdicts.push_back({ capability, o.state, o.detail });
		if (out)
		{
			fprintf(out, "hwprobe %s %s %s (%ld ms)\n", capability.c_str(), HwProbeStateName(o.state), o.detail.c_str(), ms);
			fflush(out);
		}
	};

	PictPtr gpuMire;
	judge("device", [] { return Ok("device VAAPI ouvert"); });
	judge("upload", [&] { return ProbeUpload(gpuMire); });
	judge("download", [&] { return ProbeDownload(gpuMire); });
	judge("h264.encode", [] { return ProbeH264Encode(); });

	std::string okProfiles;
	for (const auto& p : Profiles)
	{
		judge(std::string("h264.decode.") + p.plid, [&] { return ProbeH264Decode(p.plid, p.spsFlags); });
		if (verdicts.back().state == HwProbeState::Ok)
			okProfiles += (okProfiles.empty() ? "" : " ") + std::string(p.plid);
	}
	judge("h264.decode", [&] {
		return okProfiles.empty() ? Failed("aucun profil") : Ok("profils " + okProfiles);
	});

	judge("vp8.decode", [] { return ProbeVp8Decode(); });
	judge("vp8.encode", [] { return Absent("VP8Encoder encode en logiciel (libvpx)"); });
	judge("av1.decode", [] { return Absent("AV1Decoder decode par libdav1d, logiciel"); });
	judge("scale", [&] { return ProbeScale(gpuMire); });
	judge("mosaic", [&] { return ProbeMosaic(gpuMire); });
	return verdicts;
}

std::vector<HwProbeVerdict> RunHwProbeChild(int timeoutSecs)
{
	std::vector<HwProbeVerdict> received;
	// Les logs de l'enfant partagent sa sortie : on garde les derniers, qui
	// disent pourquoi il est mort (assertion de libavcodec, par exemple).
	std::deque<std::string> lastLogs;
	std::string cause;

	int fds[2];
	if (pipe2(fds, O_CLOEXEC) < 0)
		cause = std::string("pipe : ") + strerror(errno);
	pid_t pid = cause.empty() ? fork() : -1;
	if (pid == 0)
	{
		dup2(fds[1], STDOUT_FILENO);
		execl("/proc/self/exe", "mediaserver", "--hwprobe", (char*)NULL);
		_exit(127);
	}
	if (cause.empty() && pid < 0)
		cause = std::string("fork : ") + strerror(errno);

	if (pid > 0)
	{
		close(fds[1]);
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSecs);
		std::string pending;
		bool timedOut = false;
		for (;;)
		{
			long left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
			if (left <= 0)
			{
				timedOut = true;
				break;
			}
			pollfd pfd = { fds[0], POLLIN, 0 };
			if (poll(&pfd, 1, (int)left) <= 0)
				continue;
			char buf[1024];
			ssize_t n = read(fds[0], buf, sizeof(buf));
			if (n <= 0)
				break;
			pending.append(buf, n);
			size_t eol;
			while ((eol = pending.find('\n')) != std::string::npos)
			{
				std::string line = pending.substr(0, eol);
				pending.erase(0, eol + 1);
				char cap[64], state[16];
				int used = 0;
				bool known = false;
				if (line.rfind("hwprobe ", 0) == 0
				    && sscanf(line.c_str(), "hwprobe %63s %15s %n", cap, state, &used) == 2)
				{
					HwProbeState st = ParseState(state, known);
					if (known)
					{
						received.push_back({ cap, st, line.substr(used) });
						continue;
					}
				}
				lastLogs.push_back(line);
				if (lastLogs.size() > 5)
					lastLogs.pop_front();
			}
		}
		close(fds[0]);

		if (timedOut)
			kill(pid, SIGKILL);
		int status = 0;
		waitpid(pid, &status, 0);
		if (timedOut)
			cause = "delai depasse (" + std::to_string(timeoutSecs) + " s)";
		else if (WIFSIGNALED(status))
			cause = "sonde tuee par le signal " + std::to_string(WTERMSIG(status))
			      + " (" + strsignal(WTERMSIG(status)) + ")";
		else if (WEXITSTATUS(status) != 0)
			cause = "sonde sortie avec le code " + std::to_string(WEXITSTATUS(status));
	}

	std::vector<HwProbeVerdict> verdicts;
	const std::vector<std::string>& caps = HwProbeCapabilities();
	size_t next = 0;
	for (const std::string& cap : caps)
	{
		if (next < received.size() && received[next].capability == cap)
		{
			verdicts.push_back(received[next++]);
			continue;
		}
		if (cause.empty())
			cause = "verdict manquant";
		bool first = verdicts.size() == next;
		verdicts.push_back({ cap, HwProbeState::Failed, first ? cause : "non testee" });
	}

	if (verdicts.size() != next)
	{
		Error("-hwprobe: la sonde s'est arretee avant la fin : %s\n", cause.c_str());
		for (const std::string& l : lastLogs)
			Error("-hwprobe:   %s\n", l.c_str());
	}
	return verdicts;
}
