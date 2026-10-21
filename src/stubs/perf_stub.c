/* Stand-in for Amazon's libperf_cpu_info_parser.so, for an Echo whose perfmonitord is gone (scripts/debloat.sh).
 *
 * mixer (and PuffinApp) call three functions of it around audio streams.  The stock ones connect to perfmonitord's
 * AIPC service (uuid 0xb6) and send it one "dump CPU info" message; with that daemon missing, aceAipc_connect waits
 * 20 s for its server, the mixer's record thread stalls with it and the microphone delivers nothing ("micAsr delivers
 * nothing (status 110)": no wake word; measured 2026-10-21 on a Dot 3).  The stock return values are 1 = done, 0 = failed,
 * read from the disassembly of the firmware's library (NS65741).  The library's other exports are only used by
 * perfmonitord itself.  No dependencies: not even libc.
 *
 * The built file is committed as devices/donut/prebuilt/libperf_cpu_info_parser.so (1740 bytes, so that the guided setup needs
 * no toolchain for it).  To rebuild it: make DEVICE=donut build/donut/libperf_cpu_info_parser.so, then copy it over that one;
 * what it must export is the three functions below (readelf --dyn-syms), nothing else.
 */
int perfmonitord_initConnection(void) { return 1; }

int perfmonitord_dumpPerfCpuInfo(int arg) { (void)arg; return 1; }

void perfmonitord_closeConnection(void) { }
