#!/bin/sh
# Regenerates runtime/src/gl_consts.h from the devkitPro GLES2 header.
set -e
H=${1:-$DEVKITPRO/portlibs/switch/include/GLES2/gl2.h}
OUT=$(dirname "$0")/../src/gl_consts.h
{
  echo "// Generated from GLES2/gl2.h by runtime/tools/gen_gl_consts.sh. WebGL names = GL names minus \"GL_\"."
  echo "static const struct { const char *name; uint32_t value; } gl_consts[] = {"
  grep -E "^#define GL_[A-Z0-9_]+ +0x[0-9A-Fa-f]+$|^#define GL_[A-Z0-9_]+ +[0-9]+$" "$H" | grep -vE "GL_ES_VERSION_2_0|GL_GLES_PROTOTYPES" | awk '{n=substr($2,4); printf "    {\"%s\", %s},\n", n, $3}'
  printf '%s\n' '    {"UNPACK_FLIP_Y_WEBGL", 0x9240},' '    {"UNPACK_PREMULTIPLY_ALPHA_WEBGL", 0x9241},' '    {"CONTEXT_LOST_WEBGL", 0x9242},' '    {"UNPACK_COLORSPACE_CONVERSION_WEBGL", 0x9243},' '    {"BROWSER_DEFAULT_WEBGL", 0x9244},' '    {"DEPTH_STENCIL", 0x84F9},' '    {"DEPTH_STENCIL_ATTACHMENT", 0x821A},' '};'
} > "$OUT"
