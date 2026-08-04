/**
 * test_mosaic_composition.cpp — composition vidéo des mosaïques (graphe avfilter).
 *
 * Couvre les deux étages de la composition introduite par les Phases 0-3 du plan
 * `mosaic_avfilter_plan.md` :
 *
 *  1. `MosaicGeometry` — la GÉOMÉTRIE décidée par `Mosaic::BuildDesc()` : quelles
 *     vignettes, à quelle taille, à quelle position. Aucun pixel n'est produit,
 *     donc ces tests sont instantanés et couvrent les 12 types de composition.
 *  2. `MosaicComposition` — la COMPOSITION réelle par `MosaicCompositor` : on
 *     pousse des trames de luma uniforme dans les slots et on relit les pixels du
 *     composite rendu par le graphe avfilter.
 *
 * Les valeurs attendues des dispositions en grille dérivent du code historique
 * (blits BYTE* de partedmosaic/asymmetricmosaic), CORRIGÉES du liseré noir :
 * chaque slot réserve Mosaic::SlotBorder (2 px) de chaque côté, l'image se place
 * dans le slot utile ainsi réduit et le graphe la borde de noir (pad). La
 * géométrie d'un slot vaut donc l'historique décalé de +2 et rétréci de 4.
 *
 * Deux régressions vécues sont explicitement gardées ici :
 *  - `EmptySlotsAreExcludedFromGraph` : une description à **0 slot** est la
 *    signature d'« aucune trame décodée n'atteint la mosaïque » (le composite se
 *    réduit alors au fond, d'où la surface unie observée côté endpoints quand le
 *    décodeur H264 était recréé à chaque paquet RTP — cf. test_codec_type.cpp).
 *  - `SlotsAlwaysFitInsideTheComposite` : le letterbox se calcule contre le ratio
 *    du SLOT et non contre le ratio global de la mosaïque. L'ancienne formule
 *    n'était juste que pour des cellules homothétiques à la toile et débordait
 *    sinon — c'est ce qui interdisait le 1+1 pleine hauteur.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include <Magick++.h>

#include "log.h"
#include "mosaic.h"
#include "videomixer.h"

namespace {

// --- Accès à BuildDesc(), qui est protégé -----------------------------------
// Idiome du pointeur sur membre pris dans le contexte d'une classe dérivée :
// bien défini par le standard, contrairement à un cast de type. MosaicProbe
// reste abstraite (les GetWidth/GetTop... de Mosaic sont pures) et n'est jamais
// instanciée : seule sa fonction statique est utilisée.
struct MosaicProbe : public Mosaic
{
	static MosaicGraphDesc Desc(Mosaic& m)
	{
		MosaicGraphDesc (Mosaic::*fn)() = &MosaicProbe::BuildDesc;
		return (m.*fn)();
	}
};

// Trame YUV420P de luma uniforme (chrominance neutre) : après mise à l'échelle
// par le graphe, tout pixel de la vignette vaut encore `luma`, ce qui rend le
// composite vérifiable pixel à pixel.
PictPtr SolidPict(int w, int h, BYTE luma)
{
	return Pict::CreateColor(w, h, luma, 128, 128);
}

BYTE LumaAt(const PictPtr& pic, int x, int y)
{
	AVFrame* f = pic->GetAVFrame();
	return f->data[0][y * f->linesize[0] + x];
}

std::unique_ptr<Mosaic> MakeMosaic(Mosaic::Type type)
{
	// HD720P : la toile 1280x720 des conférences de test.
	return std::unique_ptr<Mosaic>(Mosaic::CreateMosaic(type, HD720P));
}

// Remplit les `n` premiers slots avec des trames inW x inH.
void FillSlots(Mosaic& m, int n, int inW, int inH, BYTE firstLuma = 40)
{
	for (int i = 0; i < n; i++)
		m.Update(i, SolidPict(inW, inH, (BYTE)(firstLuma + i * 10)));
}

// Écrit un PNG uni (opaque) et rend son chemin. Sert de contenu d'overlay
// déterministe : pas de dépendance aux fontes du système comme RenderText.
// La taille source importe peu : LoadImage étire à la taille EXACTE de
// l'overlay (Geometry.aspect), le slot utile n'étant plus 16:9 avec le liseré.
std::string WriteSolidPng(const char* name, int w, int h,
                          double r, double g, double b)
{
	std::string path = ::testing::TempDir() + name;
	Magick::Image img(Magick::Geometry(w, h), Magick::ColorRGB(r, g, b));
	img.write(path);
	return path;
}

// Luma attendue (BT.601 limité, conversion RGB->YUV du graphe) du rouge pur.
const int kRedLuma = 81;

// Rectangle attendu pour un slot : taille de la vignette et position.
struct Rect { int w, h, x, y; };

void ExpectSlots(const MosaicGraphDesc& d, const std::vector<Rect>& expected)
{
	ASSERT_EQ(expected.size(), d.slots.size());
	for (size_t i = 0; i < expected.size(); i++)
	{
		const MosaicSlotDesc& s = d.slots[i];
		const Rect& e = expected[i];
		EXPECT_EQ(e.w, s.w) << "largeur du slot " << i;
		EXPECT_EQ(e.h, s.h) << "hauteur du slot " << i;
		EXPECT_EQ(e.x, s.x) << "x du slot " << i;
		EXPECT_EQ(e.y, s.y) << "y du slot " << i;
	}
}

} // namespace

// ===========================================================================
// Fabrique : robustesse face à un type de composition invalide
// ===========================================================================
// Les deux API de contrôle (XML-RPC MCU `SetCompositionType`/`CreateMosaic` et
// JSR-309 `VideoMixerMosaicCreate`) castent un entier brut venu du réseau en
// `Mosaic::Type` sans le valider. `Mosaic::CreateMosaic` levait alors
// `throw new std::runtime_error` — un POINTEUR, qu'aucun `catch` ne peut
// intercepter — depuis un code exécuté sous le verrou de `VideoMixer` : un seul
// type erroné envoyé par le contrôleur terminait le mediaserver ENTIER, toutes
// conférences confondues.

TEST(MosaicFactory, AcceptsEveryDocumentedType)
{
	const Mosaic::Type types[] = {
		Mosaic::mosaic1x1, Mosaic::mosaic2x2, Mosaic::mosaic3x3,
		Mosaic::mosaic3p4, Mosaic::mosaic1p7, Mosaic::mosaic1p5,
		Mosaic::mosaic1p1, Mosaic::mosaicPIP1, Mosaic::mosaicPIP3,
		Mosaic::mosaic4x4, Mosaic::mosaic1p4,  Mosaic::mosaic2p8 };

	for (Mosaic::Type type : types)
	{
		Mosaic* m = nullptr;
		ASSERT_NO_THROW(m = Mosaic::CreateMosaic(type, HD720P));
		EXPECT_NE(nullptr, m) << "type documente refuse : " << (int)type;
		delete m;
	}
}

TEST(MosaicFactory, RejectsUnknownTypeWithoutThrowing)
{
	// Valeurs hors énumération : juste au-delà du dernier type, négative, absurde.
	const int invalid[] = { 12, 13, 99, -1, 1000000 };

	for (int type : invalid)
	{
		Mosaic* m = nullptr;
		ASSERT_NO_THROW(m = Mosaic::CreateMosaic((Mosaic::Type)type, HD720P))
			<< "type " << type << " : la fabrique ne doit jamais lever";
		EXPECT_EQ(nullptr, m) << "type " << type << " : doit etre refuse";
		delete m;
	}
}

TEST(MosaicFactory, GetNumSlotsForUnknownTypeIsZero)
{
	EXPECT_EQ(0, Mosaic::GetNumSlotsForType((Mosaic::Type)999));
}

// Le mixer doit refuser le type invalide ET RESTER UTILISABLE : l'ancien chemin
// levait sous `lstVideosUse.WaitUnusedAndLock()`, laissant le verrou pris. Ici on
// vérifie qu'une composition valide passe ensuite — dans un thread borné dans le
// temps, car un verrou fuité ferait PENDRE la suite au lieu de l'échouer.
TEST(MosaicFactory, MixerRejectsUnknownCompositionAndStaysUsable)
{
	VideoMixer mixer(L"test");

	EXPECT_EQ(0, mixer.SetCompositionType(0, (Mosaic::Type)999, HD720P))
		<< "un type invalide doit etre refuse proprement";
	EXPECT_LT(mixer.CreateMosaic((Mosaic::Type)999, HD720P), 0)
		<< "CreateMosaic doit signaler l'echec par un id negatif";

	// Le verrou a-t-il été relâché ? Si non, cet appel ne rendra jamais la main.
	std::future<int> valid = std::async(std::launch::async, [&mixer] {
		return mixer.SetCompositionType(0, Mosaic::mosaic2x2, HD720P);
	});

	ASSERT_EQ(std::future_status::ready, valid.wait_for(std::chrono::seconds(5)))
		<< "verrou du mixer non relache : le mixer est fige";
	EXPECT_EQ(1, valid.get());
}

// ===========================================================================
// Géométrie (BuildDesc) — aucun pixel produit
// ===========================================================================

// Le 1+1 doit utiliser TOUTE la hauteur de la toile : une source 4:3 rend
// 640x480 (et non 479x360 comme lorsque GetHeight bridait le slot à la moitié
// de la hauteur). C'est le gain de rendu demandé par le mainteneur.
TEST(MosaicGeometry, Mosaic1p1UsesFullHeightFor4x3Source)
{
	auto m = MakeMosaic(Mosaic::mosaic1p1);
	FillSlots(*m, 2, 640, 480);

	MosaicGraphDesc d = MosaicProbe::Desc(*m);
	EXPECT_EQ(1280, d.width);
	EXPECT_EQ(720,  d.height);
	// Letterbox vertical dans un slot utile de 636x716 : 636x477 centré.
	ExpectSlots(d, { {636, 477, 2, 120}, {636, 477, 642, 120} });
}

// Contrepartie du test précédent : une source 16:9 rend comme avant le passage
// aux slots pleine hauteur, au liseré près (aucune régression pour les
// endpoints HD).
TEST(MosaicGeometry, Mosaic1p1UnchangedFor16x9Source)
{
	auto m = MakeMosaic(Mosaic::mosaic1p1);
	FillSlots(*m, 2, 1280, 720);

	ExpectSlots(MosaicProbe::Desc(*m), { {636, 357, 2, 180}, {636, 357, 642, 180} });
}

// Le 1+1 a un fond NOIR (et non le gris neutre) : c'est la seule disposition
// dans ce cas, via le virtuel HasBlackBackground().
TEST(MosaicGeometry, Mosaic1p1HasBlackBackground)
{
	EXPECT_TRUE(MosaicProbe::Desc(*MakeMosaic(Mosaic::mosaic1p1)).blackBackground);
	EXPECT_FALSE(MosaicProbe::Desc(*MakeMosaic(Mosaic::mosaic2x2)).blackBackground);
}

// Plein écran : une source 4:3 est pillarboxée sur toute la hauteur.
TEST(MosaicGeometry, Grid1x1PillarboxesA4x3Source)
{
	auto m = MakeMosaic(Mosaic::mosaic1x1);
	FillSlots(*m, 1, 640, 480);

	ExpectSlots(MosaicProbe::Desc(*m), { {954, 716, 162, 2} });
}

// Non-régression des grilles : valeurs du chemin BYTE* historique.
TEST(MosaicGeometry, Grid2x2MatchesLegacyPlacement)
{
	auto m43 = MakeMosaic(Mosaic::mosaic2x2);
	FillSlots(*m43, 4, 640, 480);
	ExpectSlots(MosaicProbe::Desc(*m43), {
		{474, 356,  82,   2}, {474, 356, 722,   2},
		{474, 356,  82, 362}, {474, 356, 722, 362} });

	// Source 16:9 : la cellule utile est remplie, aucune bande (le ratio du slot
	// utile 636x356 reste à ~1% du 16:9, la tolérance du letterbox l'absorbe).
	auto m169 = MakeMosaic(Mosaic::mosaic2x2);
	FillSlots(*m169, 4, 1280, 720);
	ExpectSlots(MosaicProbe::Desc(*m169), {
		{636, 356,   2,   2}, {636, 356, 642,   2},
		{636, 356,   2, 362}, {636, 356, 642, 362} });
}

TEST(MosaicGeometry, Grid3x3MatchesLegacyPlacement)
{
	auto m = MakeMosaic(Mosaic::mosaic3x3);
	FillSlots(*m, 9, 640, 480);

	MosaicGraphDesc d = MosaicProbe::Desc(*m);
	ASSERT_EQ(9u, d.slots.size());
	for (const MosaicSlotDesc& s : d.slots)
	{
		EXPECT_EQ(314, s.w);
		EXPECT_EQ(236, s.h);
	}
	EXPECT_EQ(56,  d.slots[0].x);
	EXPECT_EQ(2,   d.slots[0].y);
	EXPECT_EQ(908, d.slots[2].x);
	EXPECT_EQ(482, d.slots[8].y);
}

// Disposition asymétrique : un grand slot + 7 vignettes.
TEST(MosaicGeometry, Asymmetric1p7MatchesLegacyPlacement)
{
	auto m = MakeMosaic(Mosaic::mosaic1p7);
	FillSlots(*m, 8, 640, 480);

	MosaicGraphDesc d = MosaicProbe::Desc(*m);
	ASSERT_EQ(8u, d.slots.size());
	EXPECT_EQ(714, d.slots[0].w);
	EXPECT_EQ(536, d.slots[0].h);
	EXPECT_EQ(122, d.slots[0].x);
	for (size_t i = 1; i < d.slots.size(); i++)
	{
		EXPECT_EQ(234, d.slots[i].w) << "vignette " << i;
		EXPECT_EQ(176, d.slots[i].h) << "vignette " << i;
	}
}

// PIP : le slot principal est ÉTIRÉ plein cadre (aspect délibérément ignoré,
// fidèle à l'historique), l'incrustation reste letterboxée.
TEST(MosaicGeometry, PIPMainSlotIsStretchedFullFrame)
{
	auto m = MakeMosaic(Mosaic::mosaicPIP1);
	FillSlots(*m, 2, 640, 480);

	MosaicGraphDesc d = MosaicProbe::Desc(*m);
	ASSERT_EQ(2u, d.slots.size());
	ExpectSlots(d, { {1276, 716, 2, 2}, {234, 176, 170, 506} });
}

// keepAspect=false : l'image remplit le slot utile, sans bande (chemin du LOGO,
// posé par CleanSlot avec KeepAspectRatio(false) : il hérite du liseré).
TEST(MosaicGeometry, KeepAspectRatioFalseFillsTheSlot)
{
	auto m = MakeMosaic(Mosaic::mosaic2x2);
	m->KeepAspectRatio(false);
	FillSlots(*m, 1, 640, 480);

	ExpectSlots(MosaicProbe::Desc(*m), { {636, 356, 2, 2} });
}

// Un slot sans trame n'a AUCUNE branche dans le graphe : le fond reste visible.
// Une description à 0 slot est la signature « aucune trame décodée » (le
// composite se réduit au fond, d'où la surface unie côté endpoints).
TEST(MosaicGeometry, EmptySlotsAreExcludedFromGraph)
{
	auto m = MakeMosaic(Mosaic::mosaic2x2);
	EXPECT_EQ(0u, MosaicProbe::Desc(*m).slots.size());

	m->Update(2, SolidPict(640, 480, 200));
	MosaicGraphDesc d = MosaicProbe::Desc(*m);
	ASSERT_EQ(1u, d.slots.size());
	EXPECT_EQ(2, d.slots[0].pos) << "le slot actif doit garder son indice";

	// Clean() libère la trame -> le slot redevient vide.
	m->Clean(2);
	EXPECT_EQ(0u, MosaicProbe::Desc(*m).slots.size());
}

// Invariant structurel : quelle que soit la disposition et le ratio de la
// source, une vignette reste dans les limites de la toile. C'est ce que
// violait le letterbox calculé sur le ratio GLOBAL dès qu'une cellule cessait
// d'être homothétique à la toile.
TEST(MosaicGeometry, SlotsAlwaysFitInsideTheComposite)
{
	const Mosaic::Type types[] = {
		Mosaic::mosaic1x1, Mosaic::mosaic2x2, Mosaic::mosaic3x3,
		Mosaic::mosaic3p4, Mosaic::mosaic1p7, Mosaic::mosaic1p5,
		Mosaic::mosaic1p1, Mosaic::mosaicPIP1, Mosaic::mosaicPIP3,
		Mosaic::mosaic4x4, Mosaic::mosaic1p4,  Mosaic::mosaic2p8 };

	// 4:3, 16:9, carré, ultra-large : force les deux branches du letterbox.
	const int sources[][2] = { {640,480}, {1280,720}, {480,480}, {1680,720} };

	for (Mosaic::Type type : types)
		for (const auto& src : sources)
		{
			auto m = MakeMosaic(type);
			const int n = Mosaic::GetNumSlotsForType(type);
			FillSlots(*m, n, src[0], src[1]);

			MosaicGraphDesc d = MosaicProbe::Desc(*m);
			SCOPED_TRACE("type=" + std::to_string((int)type) +
			             " source=" + std::to_string(src[0]) + "x" + std::to_string(src[1]));
			EXPECT_EQ((size_t)n, d.slots.size());
			for (const MosaicSlotDesc& s : d.slots)
			{
				// L'encombrement inclut le liseré : (x-b,y-b)+(w+2b,h+2b).
				EXPECT_EQ(Mosaic::SlotBorder, s.border);
				EXPECT_GT(s.w, 0);
				EXPECT_GT(s.h, 0);
				EXPECT_GE(s.x, s.border);
				EXPECT_GE(s.y, s.border);
				EXPECT_LE(s.x + s.w + s.border, d.width)
					<< "debordement horizontal, slot " << s.pos;
				EXPECT_LE(s.y + s.h + s.border, d.height)
					<< "debordement vertical, slot " << s.pos;
			}
		}
}

// Clé de reconfiguration du graphe : une nouvelle trame de MÊME géométrie ne
// doit pas reconstruire le graphe, un changement de résolution d'entrée si.
TEST(MosaicGeometry, DescIsStableAcrossContentChanges)
{
	auto m = MakeMosaic(Mosaic::mosaic2x2);
	FillSlots(*m, 2, 640, 480);
	const MosaicGraphDesc before = MosaicProbe::Desc(*m);

	// Même taille d'entrée, autre contenu : description identique.
	m->Update(0, SolidPict(640, 480, 250));
	EXPECT_TRUE(before == MosaicProbe::Desc(*m));

	// Résolution d'entrée différente : description différente (reconstruction).
	m->Update(0, SolidPict(320, 240, 250));
	EXPECT_FALSE(before == MosaicProbe::Desc(*m));
}

// Politique GPU (§2.1) : sans la moindre entrée GPU, composer sur GPU serait
// une pure perte (uploads puis probable redescente) — wantGPU doit rester
// faux, que la machine ait un device VAAPI ou non.
TEST(MosaicGeometry, WantGpuRequiresGpuInputs)
{
	auto m = MakeMosaic(Mosaic::mosaic2x2);
	FillSlots(*m, 2, 640, 480);
	EXPECT_FALSE(MosaicProbe::Desc(*m).wantGPU);
}

// ===========================================================================
// Composition réelle (MosaicCompositor + graphe avfilter) — pixels vérifiés
// ===========================================================================

TEST(MosaicComposition, ComposesEachQuadrantOf2x2)
{
	auto m = MakeMosaic(Mosaic::mosaic2x2);
	const BYTE luma[4] = { 40, 90, 160, 220 };
	for (int i = 0; i < 4; i++)
		m->Update(i, SolidPict(640, 480, luma[i]));

	PictPtr out = m->GetPict();
	ASSERT_TRUE(out != nullptr);
	ASSERT_EQ(1280u, out->GetWidth());
	ASSERT_EQ(720u,  out->GetHeight());

	// Centre de chaque image (474x356 en 82,2 / 722,2 / 82,362 / 722,362).
	EXPECT_NEAR(luma[0], LumaAt(out, 320, 180), 2);
	EXPECT_NEAR(luma[1], LumaAt(out, 960, 180), 2);
	EXPECT_NEAR(luma[2], LumaAt(out, 320, 540), 2);
	EXPECT_NEAR(luma[3], LumaAt(out, 960, 540), 2);

	// Bande gauche du premier quadrant (x<81) : le fond gris neutre reste visible.
	EXPECT_NEAR(128, LumaAt(out, 20, 180), 2);
}

// Le liseré : SlotBorder px de NOIR autour de chaque image, rendus par le pad
// du graphe — garanti noir même sur le fond gris neutre. C'est du noir vidéo
// (Y~16, limited range), on vérifie « sombre » (<=24) pour distinguer sans
// ambiguïté du fond (128) et de l'image, sans dépendre de la matrice exacte.
TEST(MosaicComposition, BlackBorderSurroundsEachImage)
{
	auto m = MakeMosaic(Mosaic::mosaic2x2);
	FillSlots(*m, 4, 640, 480, 200);

	PictPtr out = m->GetPict();
	ASSERT_TRUE(out != nullptr);

	// Image du slot 0 : 474x356 en (82,2) -> liseré sur (80,0)-(557,359).
	EXPECT_NEAR(200, LumaAt(out, 320, 180), 2);   // l'image elle-même
	EXPECT_LE(LumaAt(out,  81, 180), 24);         // liseré gauche
	EXPECT_LE(LumaAt(out, 556, 180), 24);         // liseré droit
	EXPECT_NEAR(128, LumaAt(out,  40, 180), 2);   // fond avant le liseré
	// Verticalement l'image+liseré remplissent le slot : la frontière entre les
	// quadrants (y=358..361 = liseré bas du slot 0 + liseré haut du slot 2) est
	// entièrement noire, d'où le trait qui sépare visuellement les vignettes.
	EXPECT_LE(LumaAt(out, 320,   0), 24);
	EXPECT_LE(LumaAt(out, 320, 359), 24);
	EXPECT_LE(LumaAt(out, 320, 360), 24);
}

// Le letterbox laisse voir le FOND (pas de pad noir ajouté) : sur le 1+1, dont
// le fond est noir, les bandes valent 0 et la vignette sa luma.
TEST(MosaicComposition, LetterboxBandsShowTheBackground)
{
	auto m = MakeMosaic(Mosaic::mosaic1p1);
	m->Update(0, SolidPict(640, 480, 200));

	PictPtr out = m->GetPict();
	ASSERT_TRUE(out != nullptr);
	// Image 636x477 posée en (2,120) : au-dessus (hors liseré) c'est le fond noir.
	EXPECT_NEAR(0,   LumaAt(out, 320,  60), 2);
	EXPECT_NEAR(200, LumaAt(out, 320, 300), 2);
	EXPECT_NEAR(0,   LumaAt(out, 320, 660), 2);
	// Slot 1 vide : fond noir sur toute la moitié droite.
	EXPECT_NEAR(0,   LumaAt(out, 960, 300), 2);
}

// Une seule composition par tick : plusieurs GetPict() consécutifs rendent le
// MÊME Pict (cache intra-tick), invalidé par tout Update/Clean.
TEST(MosaicComposition, CachesCompositeWithinATick)
{
	auto m = MakeMosaic(Mosaic::mosaic2x2);
	FillSlots(*m, 2, 640, 480);

	PictPtr first  = m->GetPict();
	PictPtr second = m->GetPict();
	ASSERT_TRUE(first != nullptr);
	EXPECT_EQ(first.get(), second.get()) << "le composite doit etre mis en cache";

	m->Update(0, SolidPict(640, 480, 250));
	PictPtr third = m->GetPict();
	ASSERT_TRUE(third != nullptr);
	EXPECT_NE(first.get(), third.get()) << "Update doit invalider le cache";
}

TEST(MosaicComposition, CleanRestoresTheBackground)
{
	auto m = MakeMosaic(Mosaic::mosaic2x2);
	FillSlots(*m, 4, 640, 480);
	ASSERT_TRUE(m->GetPict() != nullptr);

	m->Clean(0);
	PictPtr out = m->GetPict();
	ASSERT_TRUE(out != nullptr);
	// Quadrant nettoyé : fond gris ; les autres restent composés.
	EXPECT_NEAR(128, LumaAt(out, 320, 180), 2);
	EXPECT_NEAR(50,  LumaAt(out, 960, 180), 2);
}

// Le composite doit sortir à CHAQUE tick : pas d'EAGAIN ni de blocage du
// framesync (toutes les entrées sont poussées au même pts, cf. §4 du plan).
TEST(MosaicComposition, ProducesAFrameOnEveryTick)
{
	auto m = MakeMosaic(Mosaic::mosaic2x2);

	for (int tick = 0; tick < 30; tick++)
	{
		const BYTE luma = (BYTE)(20 + tick * 5);
		for (int i = 0; i < 4; i++)
			m->Update(i, SolidPict(640, 480, luma));

		PictPtr out = m->GetPict();
		ASSERT_TRUE(out != nullptr) << "aucun composite au tick " << tick;
		EXPECT_NEAR(luma, LumaAt(out, 320, 180), 2) << "tick " << tick;
	}
}

// Changement de résolution d'entrée en cours de conférence : le graphe est
// reconstruit et le composite reste correct (pas de trame figée ni d'erreur).
TEST(MosaicComposition, SurvivesInputResolutionChange)
{
	auto m = MakeMosaic(Mosaic::mosaic2x2);
	m->Update(0, SolidPict(640, 480, 60));
	ASSERT_TRUE(m->GetPict() != nullptr);

	m->Update(0, SolidPict(1280, 720, 210));
	PictPtr out = m->GetPict();
	ASSERT_TRUE(out != nullptr);
	// Source 16:9 : la cellule utile est désormais remplie, y compris près du
	// bord gauche (l'image commence à x=2, après le liseré).
	EXPECT_NEAR(210, LumaAt(out, 320, 180), 2);
	EXPECT_NEAR(210, LumaAt(out,  10, 180), 2);
}

// Toutes les dispositions doivent composer, y compris les plus chargées
// (4x4 et 1p4 = 16 entrées dans le graphe).
TEST(MosaicComposition, EveryCompositionTypeComposes)
{
	const Mosaic::Type types[] = {
		Mosaic::mosaic1x1, Mosaic::mosaic2x2, Mosaic::mosaic3x3,
		Mosaic::mosaic3p4, Mosaic::mosaic1p7, Mosaic::mosaic1p5,
		Mosaic::mosaic1p1, Mosaic::mosaicPIP1, Mosaic::mosaicPIP3,
		Mosaic::mosaic4x4, Mosaic::mosaic1p4,  Mosaic::mosaic2p8 };

	for (Mosaic::Type type : types)
	{
		SCOPED_TRACE("type=" + std::to_string((int)type));
		auto m = MakeMosaic(type);
		FillSlots(*m, Mosaic::GetNumSlotsForType(type), 640, 480);

		PictPtr out = m->GetPict();
		ASSERT_TRUE(out != nullptr);
		EXPECT_EQ(1280u, out->GetWidth());
		EXPECT_EQ(720u,  out->GetHeight());
		EXPECT_EQ(AV_PIX_FMT_YUV420P, out->GetAVFrame()->format)
			<< "le sink doit contraindre le composite en yuv420p";
	}
}

// ===========================================================================
// Chemin GPU (Phase 5) — repli CPU vérifiable sans carte graphique
// ===========================================================================
// Sur une machine SANS device VAAPI, demander le GPU doit replier proprement
// en CPU (une trace, pas d'échec) ; sur une machine AVEC GPU ces mêmes tests
// exercent le vrai graphe VAAPI (les assertions de format en tiennent compte).

TEST(MosaicCompositorGpu, GpuRequestStillComposesEverywhere)
{
	MosaicGraphDesc d;
	d.width   = 1280;
	d.height  = 720;
	d.wantGPU = true;

	MosaicSlotDesc s;
	s.pos = 0; s.x = 82; s.y = 2; s.w = 474; s.h = 356; s.border = 2;
	s.inW = 640; s.inH = 480; s.inFmt = AV_PIX_FMT_YUV420P;
	d.slots.push_back(s);

	MosaicCompositor comp;
	ASSERT_TRUE(comp.Configure(d)) << "le repli CPU doit rendre Configure vrai";

	PictPtr out = comp.Compose({ SolidPict(640, 480, 90) },
	                           std::vector<PictPtr>(),
	                           SolidPict(1280, 720, 128), nullptr);
	ASSERT_TRUE(out != nullptr);
	EXPECT_EQ(1280u, out->GetWidth());
	EXPECT_EQ(720u,  out->GetHeight());
	if (!Pict::GetVAAPIDevice())
	{
		// Machine sans GPU : composite CPU, pixels vérifiables.
		EXPECT_EQ(AV_PIX_FMT_YUV420P, out->GetAVFrame()->format);
		EXPECT_NEAR(90, LumaAt(out, 320, 180), 2);
	}
}

// Slots superposés (PIP) : le liseré GPU est peint dans le fond, qui serait
// masqué par l'image principale — le mode GPU doit replier en CPU MÊME sur
// une machine avec device VAAPI (sortie yuv420p garantie partout).
TEST(MosaicCompositorGpu, OverlappingSlotsComposeOnCpu)
{
	MosaicGraphDesc d;
	d.width   = 1280;
	d.height  = 720;
	d.wantGPU = true;

	MosaicSlotDesc main;
	main.pos = 0; main.x = 2; main.y = 2; main.w = 1276; main.h = 716; main.border = 2;
	main.inW = 640; main.inH = 480; main.inFmt = AV_PIX_FMT_YUV420P;
	MosaicSlotDesc pip;
	pip.pos = 1; pip.x = 170; pip.y = 506; pip.w = 234; pip.h = 176; pip.border = 2;
	pip.inW = 640; pip.inH = 480; pip.inFmt = AV_PIX_FMT_YUV420P;
	d.slots.push_back(main);
	d.slots.push_back(pip);

	MosaicCompositor comp;
	ASSERT_TRUE(comp.Configure(d));

	PictPtr out = comp.Compose({ SolidPict(640, 480, 60), SolidPict(640, 480, 220) },
	                           std::vector<PictPtr>(),
	                           SolidPict(1280, 720, 128), nullptr);
	ASSERT_TRUE(out != nullptr);
	EXPECT_EQ(AV_PIX_FMT_YUV420P, out->GetAVFrame()->format)
		<< "la superposition doit forcer le chemin CPU meme avec un GPU";
	// L'incrustation est bien posée SUR l'image principale.
	EXPECT_NEAR(60,  LumaAt(out, 640, 200), 2);
	EXPECT_NEAR(220, LumaAt(out, 280, 590), 2);
}

// ===========================================================================
// Overlays (Phase 4) — participant et mosaïque composés par le graphe
// ===========================================================================
// L'overlay participant couvre son SLOT (fidèle à l'historique
// ApplyParticipantOverlay), l'overlay mosaïque couvre la toile entière et passe
// par-dessus toutes les vignettes. Le contenu est un PNG rouge opaque : sa luma
// dans le composite est vérifiable au pixel près.

// L'overlay participant est déclaré dans la description (clé de reconfig) et
// réellement composé par-dessus la vignette de son slot.
TEST(MosaicOverlay, ParticipantOverlayCoversItsSlot)
{
	const std::string png = WriteSolidPng("ovr_red_slot.png", 64, 36, 1.0, 0.0, 0.0);

	auto m = MakeMosaic(Mosaic::mosaic2x2);
	// Participant 7 -> slot 0 (mosaicPos[0]=7), participant 8 -> slot 1.
	ASSERT_EQ(0, m->AddParticipant(7));
	ASSERT_EQ(1, m->AddParticipant(8));
	m->Update(0, SolidPict(640, 480, 60));
	m->Update(1, SolidPict(640, 480, 200));

	ASSERT_EQ(1, m->SetOverlayImage(7, png.c_str()));

	MosaicGraphDesc d = MosaicProbe::Desc(*m);
	ASSERT_EQ(2u, d.slots.size());
	EXPECT_TRUE(d.slots[0].hasOverlay);
	EXPECT_FALSE(d.slots[1].hasOverlay);
	// L'overlay couvre le slot 0 UTILE (636x356 en 2,2 : liseré déduit), pas la
	// vignette letterbox ni le liseré.
	EXPECT_EQ(2,   d.slots[0].ovX);
	EXPECT_EQ(2,   d.slots[0].ovY);
	EXPECT_EQ(636, d.slots[0].ovW);
	EXPECT_EQ(356, d.slots[0].ovH);

	PictPtr out = m->GetPict();
	ASSERT_TRUE(out != nullptr);
	// Slot 0 : rouge opaque par-dessus la vignette (y compris ses bandes).
	EXPECT_NEAR(kRedLuma, LumaAt(out, 320, 180), 6);
	EXPECT_NEAR(kRedLuma, LumaAt(out,  20, 180), 6);
	// Slot 1 : intact.
	EXPECT_NEAR(200, LumaAt(out, 960, 180), 6);
}

// SetOverlayImage/ResetOverlay doivent invalider le composite en cache
// (SetChanged) : l'effet est visible dès le GetPict suivant, et le retrait
// rend la vignette d'origine.
TEST(MosaicOverlay, SetAndResetInvalidateTheCachedComposite)
{
	const std::string png = WriteSolidPng("ovr_red_cache.png", 64, 36, 1.0, 0.0, 0.0);

	auto m = MakeMosaic(Mosaic::mosaic2x2);
	ASSERT_EQ(0, m->AddParticipant(7));
	m->Update(0, SolidPict(640, 480, 60));

	PictPtr before = m->GetPict();
	ASSERT_TRUE(before != nullptr);
	EXPECT_NEAR(60, LumaAt(before, 320, 180), 2);

	// Pose : le composite suivant doit être NOUVEAU et rouge.
	ASSERT_EQ(1, m->SetOverlayImage(7, png.c_str()));
	PictPtr with = m->GetPict();
	ASSERT_TRUE(with != nullptr);
	EXPECT_NE(before.get(), with.get()) << "SetOverlayImage doit invalider le cache";
	EXPECT_NEAR(kRedLuma, LumaAt(with, 320, 180), 6);

	// Retrait : nouveau composite, vignette d'origine restaurée.
	ASSERT_EQ(1, m->ResetOverlay(7));
	PictPtr without = m->GetPict();
	ASSERT_TRUE(without != nullptr);
	EXPECT_NE(with.get(), without.get()) << "ResetOverlay doit invalider le cache";
	EXPECT_NEAR(60, LumaAt(without, 320, 180), 2);
	EXPECT_FALSE(MosaicProbe::Desc(*m).slots[0].hasOverlay);
}

// Re-rendu d'un overlay À TAILLE CONSTANTE (ex. changement de nom affiché) :
// la description du graphe ne change pas, donc pas de reconstruction — seule
// la recomposition a lieu. C'est la clé de reconfig exigée par la Phase 4.
TEST(MosaicOverlay, SameSizeReRenderKeepsTheGraphDescription)
{
	const std::string png = WriteSolidPng("ovr_red_stable.png", 64, 36, 1.0, 0.0, 0.0);

	auto m = MakeMosaic(Mosaic::mosaic2x2);
	ASSERT_EQ(0, m->AddParticipant(7));
	m->Update(0, SolidPict(640, 480, 60));

	ASSERT_EQ(1, m->SetOverlayImage(7, png.c_str()));
	const MosaicGraphDesc before = MosaicProbe::Desc(*m);
	ASSERT_TRUE(before.slots[0].hasOverlay);

	// Nouveau contenu, même taille : description STRICTEMENT identique.
	ASSERT_EQ(1, m->SetOverlayImage(7, png.c_str()));
	EXPECT_TRUE(before == MosaicProbe::Desc(*m));
}

// L'overlay mosaïque couvre la toile entière, par-dessus toutes les vignettes.
TEST(MosaicOverlay, MosaicOverlayCoversTheWholeComposite)
{
	const std::string png = WriteSolidPng("ovr_red_full.png", 128, 72, 1.0, 0.0, 0.0);

	auto m = MakeMosaic(Mosaic::mosaic2x2);
	FillSlots(*m, 4, 640, 480);

	// id<=0 = overlay de la mosaïque elle-même (convention VideoMixer).
	ASSERT_EQ(1, m->SetOverlayImage(0, png.c_str()));
	EXPECT_TRUE(MosaicProbe::Desc(*m).hasMosaicOverlay);

	PictPtr out = m->GetPict();
	ASSERT_TRUE(out != nullptr);
	// Rouge partout : centre des vignettes ET fond.
	EXPECT_NEAR(kRedLuma, LumaAt(out, 320, 180), 6);
	EXPECT_NEAR(kRedLuma, LumaAt(out, 960, 540), 6);
	EXPECT_NEAR(kRedLuma, LumaAt(out,  20, 180), 6);

	// Retrait : la composition d'origine réapparaît.
	ASSERT_EQ(1, m->ResetOverlay(0));
	EXPECT_FALSE(MosaicProbe::Desc(*m).hasMosaicOverlay);
	PictPtr without = m->GetPict();
	ASSERT_TRUE(without != nullptr);
	EXPECT_NEAR(40, LumaAt(without, 320, 180), 2);
}

// MoveOverlays (SetCompositionType) : les overlays suivent les participants
// vers la nouvelle mosaïque et y sont composés sans nouvel appel SetOverlay.
TEST(MosaicOverlay, MoveOverlaysCarriesThemToTheNewMosaic)
{
	const std::string png = WriteSolidPng("ovr_red_move.png", 64, 36, 1.0, 0.0, 0.0);

	auto oldM = MakeMosaic(Mosaic::mosaic2x2);
	ASSERT_EQ(0, oldM->AddParticipant(7));
	ASSERT_EQ(1, oldM->SetOverlayImage(7, png.c_str()));

	auto newM = MakeMosaic(Mosaic::mosaic2x2);
	ASSERT_EQ(0, newM->AddParticipant(7));
	newM->Update(0, SolidPict(640, 480, 60));
	newM->MoveOverlays(oldM.get());

	MosaicGraphDesc d = MosaicProbe::Desc(*newM);
	ASSERT_EQ(1u, d.slots.size());
	EXPECT_TRUE(d.slots[0].hasOverlay);

	PictPtr out = newM->GetPict();
	ASSERT_TRUE(out != nullptr);
	EXPECT_NEAR(kRedLuma, LumaAt(out, 320, 180), 6);
}

// Le bandeau de nom (RenderText) doit être TRANSPARENT hors de sa boîte.
// Piège ImageMagick 6->7 vécu en recette : la sémantique du 4e canal de Color
// s'est inversée (opacity -> alpha), le fond du bandeau était devenu NOIR
// OPAQUE et masquait toute la vidéo du slot — le nom s'affichait, la vidéo non.
TEST(MosaicOverlay, TextBannerDoesNotMaskTheVideo)
{
	auto m = MakeMosaic(Mosaic::mosaic2x2);
	ASSERT_EQ(0, m->AddParticipant(7));
	m->Update(0, SolidPict(640, 480, 200));

	if (m->SetOverlayTXT(7, "Alice", 0) <= 0)
		GTEST_SKIP() << "rendu texte indisponible sur cette machine (fontes)";

	PictPtr out = m->GetPict();
	ASSERT_TRUE(out != nullptr);
	// Centre du slot, au-dessus de la boîte du bandeau : la vidéo reste visible.
	EXPECT_NEAR(200, LumaAt(out, 320, 100), 6);
	// Dans la boîte (bas du slot utile) : le gris 75 % opaque assombrit
	// nettement — preuve que le bandeau est bien posé, lui.
	EXPECT_LT(LumaAt(out, 320, 340), 190);
}

// Un overlay posé sur un participant ABSENT ne touche à rien et ne casse pas
// la composition (SetOverlayImage participant inconnu = no-op journalisé).
TEST(MosaicOverlay, OverlayForUnknownParticipantIsIgnored)
{
	const std::string png = WriteSolidPng("ovr_red_unknown.png", 64, 36, 1.0, 0.0, 0.0);

	auto m = MakeMosaic(Mosaic::mosaic2x2);
	ASSERT_EQ(0, m->AddParticipant(7));
	m->Update(0, SolidPict(640, 480, 60));

	m->SetOverlayImage(999, png.c_str());

	MosaicGraphDesc d = MosaicProbe::Desc(*m);
	ASSERT_EQ(1u, d.slots.size());
	EXPECT_FALSE(d.slots[0].hasOverlay);

	PictPtr out = m->GetPict();
	ASSERT_TRUE(out != nullptr);
	EXPECT_NEAR(60, LumaAt(out, 320, 180), 2);
}
