// shaders.h — the one ES2 shader program (PRD §7.1, D2: strict ES2 + mediump,
// no extensions). Included only by render.c; static gives internal linkage.
#ifndef SHADERS_H
#define SHADERS_H

static const char* SHADER_VS_SRC =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "attribute vec4 a_color;\n"
    "uniform mat4 u_mvp;\n"
    "varying vec2 v_uv;\n"
    "varying vec4 v_color;\n"
    "void main() {\n"
    "    v_uv = a_uv;\n"
    "    v_color = a_color;\n"
    "    gl_Position = u_mvp * vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

static const char* SHADER_FS_SRC =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "varying vec4 v_color;\n"
    "uniform sampler2D u_tex;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(u_tex, v_uv) * v_color;\n"
    "}\n";

#endif // SHADERS_H
