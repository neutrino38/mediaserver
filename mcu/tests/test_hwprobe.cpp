/**
 * test_hwprobe.cpp — sonde d'accélération matérielle (lots 0 à 2 de
 * docs/conception/HWACCEL-SONDE/SPEC.md).
 *
 * Tout tourne partout, sans GPU compris, sauf
 * HwProbe.DISABLED_ToutCeQueLeServeurUtiliseEstProuve, qui exige un GPU :
 *   ./tests/runtests --gtest_also_run_disabled_tests --gtest_filter='HwProbe.*'
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "log.h"
#include "video.h"
#include "hwprobe.h"

namespace {

std::vector<std::string> ReadLines(FILE* f)
{
	std::vector<std::string> lines;
	rewind(f);
	char buf[512];
	while (fgets(buf, sizeof(buf), f))
		lines.push_back(buf);
	return lines;
}

} // namespace

TEST(HwProbe, ChaqueCapaciteRecoitUnVerdict)
{
	FILE* out = tmpfile();
	ASSERT_TRUE(out != nullptr);
	std::vector<HwProbeVerdict> verdicts = RunHwProbe(out);
	std::vector<std::string> lines = ReadLines(out);
	fclose(out);

	ASSERT_EQ(HwProbeCapabilities().size(), verdicts.size());
	ASSERT_EQ(HwProbeCapabilities().size(), lines.size());
	const bool device = Pict::GetVAAPIDevice() != nullptr;
	for (size_t i = 0; i < verdicts.size(); i++)
	{
		const HwProbeVerdict& v = verdicts[i];
		EXPECT_EQ(HwProbeCapabilities()[i], v.capability);
		std::string prefix = "hwprobe " + v.capability + " " + HwProbeStateName(v.state) + " ";
		EXPECT_EQ(0u, lines[i].rfind(prefix, 0)) << lines[i];
		if (!device)
			EXPECT_EQ(HwProbeState::Absent, v.state) << v.capability << " sans device";
	}
	EXPECT_EQ(device ? HwProbeState::Ok : HwProbeState::Absent, verdicts[0].state);
}

TEST(HwProbe, DISABLED_ToutCeQueLeServeurUtiliseEstProuve)
{
	ASSERT_NE(Pict::GetVAAPIDevice(), nullptr) << "pas de device VAAPI";
	std::map<std::string, HwProbeVerdict> byName;
	for (const HwProbeVerdict& v : RunHwProbe(nullptr))
		byName.emplace(v.capability, v);

	for (const char* cap : { "device", "upload", "download", "h264.encode",
	                         "h264.decode.42e01f", "h264.decode.42801F", "scale", "mosaic" })
		EXPECT_EQ(HwProbeState::Ok, byName.at(cap).state) << cap << " : " << byName.at(cap).detail;
}

// --- Lot 1 : la sonde relancée hors processus ------------------------------

namespace {

// Pose MCU_HWPROBE_FAULT le temps d'un test : l'enfant en hérite au fork.
struct FaultScope
{
	explicit FaultScope(const char* fault) { setenv("MCU_HWPROBE_FAULT", fault, 1); }
	~FaultScope() { unsetenv("MCU_HWPROBE_FAULT"); }
};

size_t IndexOf(const std::string& capability)
{
	const std::vector<std::string>& caps = HwProbeCapabilities();
	return std::find(caps.begin(), caps.end(), capability) - caps.begin();
}

} // namespace

TEST(HwProbeChild, RendLesMemesVerdictsQueLaSonde)
{
	std::vector<HwProbeVerdict> inProcess = RunHwProbe(nullptr);
	std::vector<HwProbeVerdict> child = RunHwProbeChild(10);

	ASSERT_EQ(HwProbeCapabilities().size(), child.size());
	ASSERT_EQ(inProcess.size(), child.size());
	for (size_t i = 0; i < child.size(); i++)
	{
		EXPECT_EQ(inProcess[i].capability, child[i].capability);
		EXPECT_EQ(inProcess[i].state, child[i].state) << child[i].capability << " : " << child[i].detail;
	}
}

// Le cas qui justifie l'ADR 002 : le driver tue la sonde en pleine capacité.
TEST(HwProbeChild, UneSondeTueeGardeSesVerdictsEtNommeLaCause)
{
	FaultScope fault("abort:h264.encode");
	std::vector<HwProbeVerdict> v = RunHwProbeChild(10);

	ASSERT_EQ(HwProbeCapabilities().size(), v.size());
	const size_t killed = IndexOf("h264.encode");
	const HwProbeState before = Pict::GetVAAPIDevice() ? HwProbeState::Ok : HwProbeState::Absent;
	for (size_t i = 0; i < killed; i++)
		EXPECT_EQ(before, v[i].state) << v[i].capability << " : verdict ecrit avant la mort, perdu";
	EXPECT_EQ(HwProbeState::Failed, v[killed].state);
	EXPECT_NE(std::string::npos, v[killed].detail.find("tuee par le signal " + std::to_string(SIGABRT)))
		<< v[killed].detail;
	for (size_t i = killed + 1; i < v.size(); i++)
	{
		EXPECT_EQ(HwProbeState::Failed, v[i].state) << v[i].capability;
		EXPECT_EQ("non testee", v[i].detail) << v[i].capability;
	}
}

// Un driver bloqué ne doit pas bloquer le démarrage du serveur.
TEST(HwProbeChild, UneSondeBloqueeEstTueeAuDelai)
{
	FaultScope fault("hang:scale");
	auto start = std::chrono::steady_clock::now();
	std::vector<HwProbeVerdict> v = RunHwProbeChild(3);
	double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

	EXPECT_LT(secs, 5.0) << "le delai n'a pas ete tenu";
	ASSERT_EQ(HwProbeCapabilities().size(), v.size());
	const size_t hung = IndexOf("scale");
	EXPECT_NE(HwProbeState::Failed, v[hung - 1].state) << v[hung - 1].capability << " : " << v[hung - 1].detail;
	EXPECT_EQ(HwProbeState::Failed, v[hung].state);
	EXPECT_EQ("delai depasse (3 s)", v[hung].detail);
	EXPECT_EQ("non testee", v[hung + 1].detail);
}

// --- Lot 2 : les verdicts décident -------------------------------------------

namespace {

// Tout `ok`, sauf les capacités citées, en échec.
std::vector<HwProbeVerdict> VerdictsFailing(std::initializer_list<const char*> failed)
{
	std::vector<HwProbeVerdict> v;
	for (const std::string& cap : HwProbeCapabilities())
		v.push_back({ cap, HwProbeState::Ok, "" });
	for (const char* f : failed)
		v[IndexOf(f)].state = HwProbeState::Failed;
	return v;
}

int DeviceFailureDisablesGpu(const char* capability)
{
	ApplyHwProbe(VerdictsFailing({ capability }));
	return Pict::GetVAAPIDevice() == nullptr ? 0 : 1;
}

int EachFailureRefusesItsOwnPath()
{
	const bool device = Pict::GetVAAPIDevice() != nullptr;
	ApplyHwProbe(VerdictsFailing({ "h264.encode", "h264.decode.42801F", "scale", "mosaic" }));
	for (const char* refused : { "h264.encode", "h264.decode.baseline", "scale", "mosaic" })
		if (!VideoAccel::IsHwRefused(refused))
			return 1;
	for (const char* kept : { "h264.decode", "h264.decode.constrained_baseline", "vp8.decode" })
		if (VideoAccel::IsHwRefused(kept))
			return 2;
	return (Pict::GetVAAPIDevice() != nullptr) == device ? 0 : 3;
}

} // namespace

// Un refus ou une extinction est définitif pour le processus : sous-processus,
// relancé (threadsafe) plutôt que forké d'un parent qui a peut-être touché au GPU.
TEST(HwProbeApply, UnEchecDuDeviceEteintLeGpu)
{
	GTEST_FLAG_SET(death_test_style, "threadsafe");
	EXPECT_EXIT(exit(DeviceFailureDisablesGpu("device")), ::testing::ExitedWithCode(0), "");
}

TEST(HwProbeApply, UnEchecDeTransfertEteintLeGpu)
{
	GTEST_FLAG_SET(death_test_style, "threadsafe");
	EXPECT_EXIT(exit(DeviceFailureDisablesGpu("download")), ::testing::ExitedWithCode(0), "");
}

TEST(HwProbeApply, ChaqueEchecNeRefuseQueSonChemin)
{
	GTEST_FLAG_SET(death_test_style, "threadsafe");
	EXPECT_EXIT(exit(EachFailureRefusesItsOwnPath()), ::testing::ExitedWithCode(0), "");
}
