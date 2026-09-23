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

// Juge chaque capacité du §3, dans l'ordre, et écrit sur `out` une ligne
// « hwprobe <capacité> <état> <détail> » dès qu'un verdict tombe.
std::vector<HwProbeVerdict> RunHwProbe(FILE* out);

#endif
