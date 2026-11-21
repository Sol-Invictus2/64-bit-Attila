#pragma once
typedef struct { const char *name; void *wrapper; void **real; } GlFloatBridge;
extern const GlFloatBridge g_gl_float_bridges[];
// Functions with no floating point and at most 6 arguments (from the OpenGL headers), ending with 0: they can use the short bridge.
extern const char *const g_gl_short_names[];
