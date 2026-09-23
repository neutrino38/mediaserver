/**
 * test_hwprobe.cpp — sonde d'accélération matérielle (lot 0 de
 * docs/conception/HWACCEL-SONDE/SPEC.md).
 *
 * HwProbe.ChaqueCapaciteRecoitUnVerdict tourne partout : sans GPU, tout est
 * `absent`. HwProbe.DISABLED_ToutCeQueLeServeurUtiliseEstProuve exige un GPU :
 *   ./tests/runtests --gtest_also_run_disabled_tests --gtest_filter='HwProbe.*'
 */
#include <gtest/gtest.h>

#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "log.h"
#include "video.h"
#include "hwprobe.h"

namespace {

const std::vector<std::string> kCapabilities = {
	"device", "upload", "download", "h264.encode",
	"h264.decode.42e01f", "h264.decode.42801F", "h264.decode.4d001f", "h264.decode.64001f",
	"h264.decode", "vp8.decode", "vp8.encode", "av1.decode", "scale", "mosaic",
};

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

	ASSERT_EQ(kCapabilities.size(), verdicts.size());
	ASSERT_EQ(kCapabilities.size(), lines.size());
	const bool device = Pict::GetVAAPIDevice() != nullptr;
	for (size_t i = 0; i < verdicts.size(); i++)
	{
		const HwProbeVerdict& v = verdicts[i];
		EXPECT_EQ(kCapabilities[i], v.capability);
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
