#ifndef ZOD_PROF_H
#define ZOD_PROF_H
/* a PC profile of the -B benchmark (ZOD_PROFILE=<file>; see zod_prof.cpp) */

#ifdef __cplusplus
extern "C" {
#endif

void zod_prof_start(void);
void zod_prof_stop(void);

/* each frame of the -B run with its seconds so far: starts the sampler once
 * ZOD_PROFILE_SKIP seconds (default 15: the map download and the first
 * uploads) have passed */
void zod_prof_tick(double run_seconds);

#ifdef __cplusplus
}
#endif

#endif
