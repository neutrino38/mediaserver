#ifndef _HWPROBE_H_
#define _HWPROBE_H_

#include <stdio.h>
#include <string>
#include <vector>

// Sonde d'accélération matérielle (docs/conception/HWACCEL-SONDE/SPEC.md).
enum class HwProbeState { Ok, Absent, Failed };

struct HwProbeVerdict
{
	std::string   capability;
	HwProbeState  state;
	std::string   detail;
};

const char* HwProbeStateName(HwProbeState state);

// Les capacités, dans l'ordre où la sonde les juge.
const std::vector<std::string>& HwProbeCapabilities();

// Juge chaque capacité du §3, dans l'ordre, et écrit sur `out` une ligne
// « hwprobe <capacité> <état> <détail> » dès qu'un verdict tombe.
// MCU_HWPROBE_FAULT=abort:<capacité> ou hang:<capacité> simule, pour les tests,
// une sonde tuée ou bloquée par le driver au moment de juger cette capacité.
std::vector<HwProbeVerdict> RunHwProbe(FILE* out);

// Lance « /proc/self/exe --hwprobe » et lit ses verdicts, au plus timeoutSecs
// secondes (ADR 002). Si la sonde meurt ou dépasse le délai, la capacité en
// cours est en échec avec la cause pour motif, et les suivantes « non testee ».
std::vector<HwProbeVerdict> RunHwProbeChild(int timeoutSecs);

#endif
