/*
 * The engine's GL (upstream zsdl_opengl.*) on AmigaOS: minigl.library 29.1,
 * through its client SDK (proto/minigl.h; MiniGLOpen at run time).
 *
 * Every file of this port that needs GL includes this header, not
 * proto/minigl.h directly: MiniGL's mgl/gl.h defines a debug macro
 * dprintf(x), and newer libnix stdio.h declares the real dprintf(fd, ...).
 * stdio.h comes first here, and the macro goes after MiniGL.
 */
#ifndef ZOD_SDL_OPENGL_H
#define ZOD_SDL_OPENGL_H

#include <stdio.h>
#include <proto/minigl.h>

#undef dprintf

#endif
