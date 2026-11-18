#ifndef ZOD_PROF_H
#define ZOD_PROF_H
/* a PC profile of the -B benchmark (ZOD_PROFILE=<file>; see zod_prof.cpp) */

#ifdef __cplusplus
extern "C" {
#endif

void zod_prof_start(void);
void zod_prof_stop(void);

#ifdef __cplusplus
}
#endif

#endif
