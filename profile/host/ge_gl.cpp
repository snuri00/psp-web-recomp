#include "ge_gl.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include <algorithm>
#include <chrono>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>

namespace pspweb {
namespace {

constexpr std::uint32_t kVramSize = 0x200000u;
constexpr std::uint32_t kNotVram = std::numeric_limits<std::uint32_t>::max();
constexpr std::size_t kMaxTextures = 4096u;
constexpr std::uint32_t kMaxSurfaceHeight = 512u;
constexpr std::uint32_t kMaxSurfaceStride = 1024u;
constexpr GLsizei kStagingWidth = 2048, kStagingHeight = 512;
constexpr std::size_t kIndexBufferPool = 512u;

const char *kVertexShader = R"(#version 300 es
layout(location = 0) in vec4 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;
uniform vec2 u_target;
uniform vec2 u_origin;
uniform vec2 u_fog;
out vec2 v_uv;
out vec4 v_color;
out float v_fog;
void main() {
    float w = a_pos.w;
    v_fog = w * u_fog.x + u_fog.y;
    vec2 ndc = (a_pos.xy + u_origin) / u_target * 2.0 - 1.0;
    float z = clamp(a_pos.z / 65535.0, 0.0, 1.0) * 2.0 - 1.0;
    gl_Position = vec4(ndc * w, z * w, w);
    v_uv = a_uv;
    v_color = a_color;
}
)";

const char *kFragmentShader = R"(#version 300 es
precision highp float;
precision highp int;
uniform sampler2D u_tex;
uniform bool u_tex_enable;
uniform vec2 u_tex_size;
uniform vec2 u_tex_origin;
uniform int u_tex_func;
uniform bool u_tex_alpha;
uniform bool u_tex_double;
uniform vec3 u_env;
uniform bool u_alpha_test;
uniform int u_alpha_func;
uniform int u_alpha_ref;
uniform int u_alpha_mask;
uniform vec3 u_src_scale;
uniform bool u_clear;
uniform vec2 u_tex_window; // PSP texture size when sampling part of a larger surface (else 0)
uniform vec2 u_tex_clamp;  // 1 = clamp, 0 = repeat, per axis
uniform bool u_tex_opaque; // sampling a 16-bit 565 surface: no alpha
uniform bool u_fog_enable;
uniform vec3 u_fog_color;
in vec2 v_uv;
in vec4 v_color;
in float v_fog;
out vec4 o_color;
bool compare(int f, int a, int b) {
    if (f == 0) return false;
    if (f == 1) return true;
    if (f == 2) return a == b;
    if (f == 3) return a != b;
    if (f == 4) return a < b;
    if (f == 5) return a <= b;
    if (f == 6) return a > b;
    return a >= b;
}
void main() {
    vec4 c = v_color;
    if (u_clear) {
        o_color = c;
        return;
    }
    if (u_tex_enable) {
        vec2 uv = v_uv;
        if (u_tex_window.x > 0.0) {
            // Wrap or clamp inside the texture's own rectangle, never into neighbouring pixels.
            uv = mix(mod(uv, u_tex_window), clamp(uv, vec2(0.5), u_tex_window - 0.5), u_tex_clamp);
        }
        vec4 t = texture(u_tex, (uv + u_tex_origin) / u_tex_size);
        if (u_tex_opaque) t.a = 1.0;
        if (u_tex_func == 0) {
            c.rgb *= t.rgb;
            if (u_tex_alpha) c.a *= t.a;
        } else if (u_tex_func == 1) {
            c.rgb = u_tex_alpha ? mix(c.rgb, t.rgb, t.a) : t.rgb;
        } else if (u_tex_func == 2) {
            c.rgb = mix(c.rgb, u_env, t.rgb);
            if (u_tex_alpha) c.a *= t.a;
        } else if (u_tex_func == 3) {
            c.rgb = t.rgb;
            if (u_tex_alpha) c.a = t.a;
        } else {
            c.rgb += t.rgb;
            if (u_tex_alpha) c.a *= t.a;
        }
        if (u_tex_double) c.rgb *= 2.0;
        c = clamp(c, 0.0, 1.0);
    }
    if (u_alpha_test) {
        int a = int(c.a * 255.0 + 0.5) & u_alpha_mask;
        if (!compare(u_alpha_func, a, u_alpha_ref)) discard;
    }
    if (u_fog_enable) c.rgb = mix(u_fog_color, c.rgb, clamp(v_fog, 0.0, 1.0));
    c.rgb *= u_src_scale;
    o_color = c;
}
)";

const char *kReinterpretVertexShader = R"(#version 300 es
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Rebuilds a surface's pixels from another surface sharing the same VRAM bytes
// in a different pixel size: 32-bit source -> 16-bit target (mode 0) or back.
const char *kReinterpretFragmentShader = R"(#version 300 es
precision highp float;
precision highp int;
uniform highp sampler2D u_src;
uniform int u_mode;
uniform int u_format;
uniform int u_delta;
uniform int u_row_bytes;
uniform ivec2 u_src_size;
uniform int u_scale;
out vec4 o_color;
uint pack16(vec4 c, int f) {
    uvec4 v = uvec4(clamp(c, 0.0, 1.0) * 255.0 + 0.5);
    if (f == 0) return (v.r >> 3) | ((v.g >> 2) << 5) | ((v.b >> 3) << 11);
    if (f == 1) return (v.r >> 3) | ((v.g >> 3) << 5) | ((v.b >> 3) << 10) | ((v.a >> 7) << 15);
    return (v.r >> 4) | ((v.g >> 4) << 4) | ((v.b >> 4) << 8) | ((v.a >> 4) << 12);
}
vec4 unpack16(uint v, int f) {
    if (f == 0) return vec4(float(v & 31u) / 31.0, float((v >> 5) & 63u) / 63.0, float((v >> 11) & 31u) / 31.0, 1.0);
    if (f == 1) return vec4(float(v & 31u) / 31.0, float((v >> 5) & 31u) / 31.0, float((v >> 10) & 31u) / 31.0,
                            float((v >> 15) & 1u));
    return vec4(float(v & 15u), float((v >> 4) & 15u), float((v >> 8) & 15u), float((v >> 12) & 15u)) / 15.0;
}
void main() {
    // Work out the byte mapping at native size; at higher internal resolutions
    // each output pixel reads the matching sub-pixel of its source pixel.
    ivec2 scaled = ivec2(gl_FragCoord.xy);
    ivec2 p = scaled / u_scale;
    ivec2 sub = scaled - p * u_scale;
    int target_bpp = u_mode == 0 ? 2 : 4;
    int rel = u_delta + p.y * u_row_bytes + p.x * target_bpp;
    if (rel < 0) discard;
    int sy = rel / u_row_bytes;
    int rem = rel - sy * u_row_bytes;
    if (sy >= u_src_size.y) discard;
    if (u_mode == 0) {
        uvec4 b = uvec4(texelFetch(u_src, ivec2(rem / 4, sy) * u_scale + sub, 0) * 255.0 + 0.5);
        uint half16 = (rem & 2) == 0 ? (b.r | (b.g << 8)) : (b.b | (b.a << 8));
        o_color = unpack16(half16, u_format);
    } else {
        int sx = rem / 2;
        uint lo = pack16(texelFetch(u_src, ivec2(sx, sy) * u_scale + sub, 0), u_format);
        uint hi = pack16(texelFetch(u_src, ivec2(min(sx + 1, u_src_size.x - 1), sy) * u_scale + sub, 0), u_format);
        o_color = vec4(float(lo & 255u), float(lo >> 8), float(hi & 255u), float(hi >> 8)) / 255.0;
    }
}
)";

// Stencil emulation: sets one stencil bit wherever that bit of the colour
// alpha is set (drawn into a framebuffer holding only the depth/stencil buffer).
const char *kAlphaToStencilShader = R"(#version 300 es
precision highp float;
precision highp int;
uniform highp sampler2D u_src;
uniform int u_bit;
out vec4 o_color;
void main() {
    int a = int(texelFetch(u_src, ivec2(gl_FragCoord.xy), 0).a * 255.0 + 0.5);
    if (((a >> u_bit) & 1) == 0) discard;
    o_color = vec4(0.0);
}
)";

// ...and the other way: adds one bit's value to the alpha where the stencil has it.
const char *kStencilToAlphaShader = R"(#version 300 es
precision highp float;
uniform float u_value;
out vec4 o_color;
void main() { o_color = vec4(0.0, 0.0, 0.0, u_value); }
)";

const char *kPresentVertexShader = R"(#version 300 es
uniform vec2 u_rect;
uniform vec2 u_origin;
out vec2 v_uv;
void main() {
    vec2 p = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    v_uv = u_origin + vec2(p.x, 1.0 - p.y) * u_rect;
}
)";

const char *kPresentFragmentShader = R"(#version 300 es
precision mediump float;
uniform sampler2D u_tex;
in vec2 v_uv;
out vec4 o_color;
void main() { o_color = vec4(texture(u_tex, v_uv).rgb, 1.0); }
)";

GLuint compile(GLenum type, const char *source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(shader, sizeof log, nullptr, log);
        std::cerr << "[gl] shader error: " << log << "\n";
    }
    return shader;
}

GLuint link(const char *vs, const char *fs) {
    const GLuint program = glCreateProgram();
    const GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    glAttachShader(program, v);
    glAttachShader(program, f);
    glLinkProgram(program);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(program, sizeof log, nullptr, log);
        std::cerr << "[gl] link error: " << log << "\n";
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return program;
}

// Guest address -> VRAM offset, or kNotVram.
std::uint32_t vram_offset(std::uint32_t address) {
    const std::uint32_t c = address & 0x1FFFFFFFu;
    if (c < 0x04000000u || c >= 0x04800000u) return kNotVram;
    return (c - 0x04000000u) & (kVramSize - 1u);
}

GLenum stencil_op(std::uint32_t op) {
    static constexpr GLenum table[6] = {GL_KEEP, GL_ZERO, GL_REPLACE, GL_INVERT, GL_INCR, GL_DECR};
    return op < 6u ? table[op] : GL_KEEP;
}

bool is_dst_alpha_factor(std::uint32_t f) { return f == 4u || f == 5u || f == 8u || f == 9u; }
bool is_src_alpha_factor(std::uint32_t f) { return f == 2u || f == 3u || f == 6u || f == 7u; }

GLenum depth_func(std::uint32_t f) {
    static constexpr GLenum table[8] = {GL_NEVER, GL_ALWAYS, GL_EQUAL, GL_NOTEQUAL, GL_LESS, GL_LEQUAL, GL_GREATER, GL_GEQUAL};
    return table[f & 7u];
}

GLenum blend_factor(std::uint32_t f, bool source, std::uint32_t fixed, bool &uses_constant) {
    switch (f) {
    case 0: return source ? GL_DST_COLOR : GL_SRC_COLOR;
    case 1: return source ? GL_ONE_MINUS_DST_COLOR : GL_ONE_MINUS_SRC_COLOR;
    case 2: case 6: return GL_SRC_ALPHA;
    case 3: case 7: return GL_ONE_MINUS_SRC_ALPHA;
    case 4: case 8: return GL_DST_ALPHA;
    case 5: case 9: return GL_ONE_MINUS_DST_ALPHA;
    default:
        if ((fixed & 0xFFFFFFu) == 0u) return GL_ZERO;
        if ((fixed & 0xFFFFFFu) == 0xFFFFFFu) return GL_ONE;
        if (source) return GL_ONE; // the shader pre-multiplies by FIXA
        uses_constant = true;
        return GL_CONSTANT_COLOR;
    }
}

std::uint32_t to_rgba(std::uint32_t v, std::uint32_t format) {
    auto e5 = [](std::uint32_t x) { return (x << 3u) | (x >> 2u); };
    auto e6 = [](std::uint32_t x) { return (x << 2u) | (x >> 4u); };
    switch (format) {
    case 0: return e5(v & 31u) | (e6((v >> 5u) & 63u) << 8u) | (e5((v >> 11u) & 31u) << 16u) | 0xFF000000u;
    case 1: return e5(v & 31u) | (e5((v >> 5u) & 31u) << 8u) | (e5((v >> 10u) & 31u) << 16u) | ((v >> 15u) ? 0xFF000000u : 0u);
    default: return ((v & 15u) * 17u) | ((((v >> 4u) & 15u) * 17u) << 8u) | ((((v >> 8u) & 15u) * 17u) << 16u) |
                    ((((v >> 12u) & 15u) * 17u) << 24u);
    }
}

std::uint32_t from_rgba(std::uint32_t rgba, std::uint32_t format) {
    const std::uint32_t r = rgba & 0xFFu, g = (rgba >> 8u) & 0xFFu, b = (rgba >> 16u) & 0xFFu, a = rgba >> 24u;
    switch (format) {
    case 0: return (r >> 3u) | ((g >> 2u) << 5u) | ((b >> 3u) << 11u);
    case 1: return (r >> 3u) | ((g >> 3u) << 5u) | ((b >> 3u) << 10u) | ((a >> 7u) << 15u);
    default: return (r >> 4u) | ((g >> 4u) << 4u) | ((b >> 4u) << 8u) | ((a >> 4u) << 12u);
    }
}

double clock_ms() {
#ifdef __EMSCRIPTEN__
    return emscripten_get_now();
#else
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

// PSPWEB_PASS_FRAME=n logs the extra GL passes of frame n.
const std::uint32_t g_pass_frame = [] {
    const char *v = std::getenv("PSPWEB_PASS_FRAME");
    return v != nullptr ? static_cast<std::uint32_t>(std::strtoul(v, nullptr, 10)) : 0u;
}();

} // namespace

GeGl::GeGl(psprecomp::GuestMemory &memory) : memory_(memory) { batch_.reserve(65536u); }

GeGl::~GeGl() = default;

void GeGl::Rect::add(int ax0, int ay0, int ax1, int ay1) {
    if (ax1 < ax0 || ay1 < ay0) return;
    if (empty()) {
        x0 = ax0; y0 = ay0; x1 = ax1; y1 = ay1;
        return;
    }
    x0 = std::min(x0, ax0); y0 = std::min(y0, ay0);
    x1 = std::max(x1, ax1); y1 = std::max(y1, ay1);
}

void GeGl::mark_drawn(Surface &s, const Rect &r) {
    const Rect c = r.clip(Rect{0, 0, static_cast<int>(s.stride) - 1, static_cast<int>(s.height) - 1});
    if (c.empty()) return;
    s.content.add(c);
    s.gpu_dirty.add(c);
    if (s.rows.size() < s.height) s.rows.resize(s.height);
    for (int y = c.y0; y <= c.y1; ++y) {
        auto &row = s.rows[static_cast<std::size_t>(y)];
        if (row.cx1 < row.cx0) { row.cx0 = c.x0; row.cx1 = c.x1; }
        else { row.cx0 = std::min(row.cx0, c.x0); row.cx1 = std::max(row.cx1, c.x1); }
        if (row.dx1 < row.dx0) { row.dx0 = c.x0; row.dx1 = c.x1; }
        else { row.dx0 = std::min(row.dx0, c.x0); row.dx1 = std::max(row.dx1, c.x1); }
    }
}

void GeGl::recompute_bounds(Surface &s) {
    s.content = Rect{};
    s.gpu_dirty = Rect{};
    for (std::size_t y = 0; y < s.rows.size(); ++y) {
        const auto &row = s.rows[y];
        if (row.cx0 <= row.cx1) s.content.add(row.cx0, static_cast<int>(y), row.cx1, static_cast<int>(y));
        if (row.dx0 <= row.dx1) s.gpu_dirty.add(row.dx0, static_cast<int>(y), row.dx1, static_cast<int>(y));
    }
}

template <typename F>
void GeGl::for_each_segment(const Surface &s, const TransferRect &rect, F &&f) const {
    const std::uint32_t base = vram_offset(rect.base);
    if (!s.alive || base == kNotVram || rect.width == 0u || rect.height == 0u) return;
    const std::uint32_t bpp = bpp_of(s.format), rb = row_bytes(s), end = surface_end(s);
    const std::uint32_t span_first = base + (rect.y * rect.stride + rect.x) * rect.bpp;
    const std::uint32_t span_last = base + ((rect.y + rect.height - 1u) * rect.stride + rect.x + rect.width) * rect.bpp;
    if (span_last <= s.offset || span_first >= end) return;
    // Byte runs of the copy, with touching rows coalesced (texture uploads are
    // usually one linear run), split into surface rows.
    auto emit = [&](std::uint32_t start, std::uint32_t stop) {
        if (stop <= s.offset || start >= end) return;
        std::uint32_t from = std::max(start, s.offset) - s.offset;
        const std::uint32_t to = std::min(stop, end) - s.offset; // exclusive
        while (from < to) {
            const std::uint32_t row = from / rb, row_end = std::min(to, (row + 1u) * rb);
            f(static_cast<int>(row), static_cast<int>((from % rb) / bpp), static_cast<int>(((row_end - 1u) % rb) / bpp));
            from = row_end;
        }
    };
    std::uint32_t run_start = 0, run_stop = 0;
    for (std::uint32_t r = 0; r < rect.height; ++r) {
        const std::uint32_t start = base + ((rect.y + r) * rect.stride + rect.x) * rect.bpp;
        const std::uint32_t stop = start + rect.width * rect.bpp;
        if (r != 0u && start == run_stop) {
            run_stop = stop;
            continue;
        }
        if (r != 0u) emit(run_start, run_stop);
        run_start = start;
        run_stop = stop;
    }
    emit(run_start, run_stop);
}

void GeGl::use_program(GLuint program) {
    if (cache_.program == program) return;
    glUseProgram(program);
    cache_.program = program;
    for (bool &valid : cache_.uniform_valid) valid = false;
}

void GeGl::bind_framebuffer(GLuint fbo) {
    if (cache_.fbo == fbo) return;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    cache_.fbo = fbo;
}

void GeGl::bind_texture0(GLuint texture) {
    if (cache_.texture == texture) return;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    cache_.texture = texture;
}

void GeGl::set_enabled(GLenum cap, int &cached, bool on) {
    if (cached == static_cast<int>(on)) return;
    if (on) glEnable(cap);
    else glDisable(cap);
    cached = static_cast<int>(on);
}

void GeGl::set_uniform(GLint location, float a, float b, float c, int count) {
    if (location < 0) return;
    if (location < 32) {
        float *v = cache_.uniforms[location];
        if (cache_.uniform_valid[location] && v[0] == a && v[1] == b && v[2] == c) return;
        v[0] = a; v[1] = b; v[2] = c;
        cache_.uniform_valid[location] = true;
    }
    if (count == 1) glUniform1f(location, a);
    else if (count == 2) glUniform2f(location, a, b);
    else glUniform3f(location, a, b, c);
}

void GeGl::set_uniform_int(GLint location, int value) {
    if (location < 0) return;
    if (location < 32) {
        float *v = cache_.uniforms[location];
        const auto as_float = static_cast<float>(value);
        if (cache_.uniform_valid[location] && v[0] == as_float) return;
        v[0] = as_float;
        cache_.uniform_valid[location] = true;
    }
    glUniform1i(location, value);
}

bool GeGl::init() {
    program_ = link(kVertexShader, kFragmentShader);
    present_program_ = link(kPresentVertexShader, kPresentFragmentShader);
    u_target_ = glGetUniformLocation(program_, "u_target");
    u_origin_ = glGetUniformLocation(program_, "u_origin");
    u_tex_ = glGetUniformLocation(program_, "u_tex");
    u_tex_enable_ = glGetUniformLocation(program_, "u_tex_enable");
    u_tex_size_ = glGetUniformLocation(program_, "u_tex_size");
    u_tex_origin_ = glGetUniformLocation(program_, "u_tex_origin");
    u_tex_func_ = glGetUniformLocation(program_, "u_tex_func");
    u_tex_alpha_ = glGetUniformLocation(program_, "u_tex_alpha");
    u_tex_double_ = glGetUniformLocation(program_, "u_tex_double");
    u_env_ = glGetUniformLocation(program_, "u_env");
    u_alpha_test_ = glGetUniformLocation(program_, "u_alpha_test");
    u_alpha_func_ = glGetUniformLocation(program_, "u_alpha_func");
    u_alpha_ref_ = glGetUniformLocation(program_, "u_alpha_ref");
    u_alpha_mask_ = glGetUniformLocation(program_, "u_alpha_mask");
    u_src_scale_ = glGetUniformLocation(program_, "u_src_scale");
    u_clear_ = glGetUniformLocation(program_, "u_clear");
    u_tex_window_ = glGetUniformLocation(program_, "u_tex_window");
    u_tex_clamp_ = glGetUniformLocation(program_, "u_tex_clamp");
    u_tex_opaque_ = glGetUniformLocation(program_, "u_tex_opaque");
    u_fog_ = glGetUniformLocation(program_, "u_fog");
    u_fog_enable_ = glGetUniformLocation(program_, "u_fog_enable");
    u_fog_color_ = glGetUniformLocation(program_, "u_fog_color");
    u_present_tex_ = glGetUniformLocation(present_program_, "u_tex");
    u_present_rect_ = glGetUniformLocation(present_program_, "u_rect");
    u_present_origin_ = glGetUniformLocation(present_program_, "u_origin");
    reinterpret_program_ = link(kReinterpretVertexShader, kReinterpretFragmentShader);
    u_ri_src_ = glGetUniformLocation(reinterpret_program_, "u_src");
    u_ri_mode_ = glGetUniformLocation(reinterpret_program_, "u_mode");
    u_ri_format_ = glGetUniformLocation(reinterpret_program_, "u_format");
    u_ri_delta_ = glGetUniformLocation(reinterpret_program_, "u_delta");
    u_ri_row_bytes_ = glGetUniformLocation(reinterpret_program_, "u_row_bytes");
    u_ri_src_size_ = glGetUniformLocation(reinterpret_program_, "u_src_size");
    u_ri_scale_ = glGetUniformLocation(reinterpret_program_, "u_scale");
    stencil_program_ = link(kReinterpretVertexShader, kAlphaToStencilShader);
    u_st_src_ = glGetUniformLocation(stencil_program_, "u_src");
    u_st_bit_ = glGetUniformLocation(stencil_program_, "u_bit");
    alpha_program_ = link(kReinterpretVertexShader, kStencilToAlphaShader);
    u_al_value_ = glGetUniformLocation(alpha_program_, "u_value");
    glUseProgram(program_);
    glUniform1i(u_tex_, 0);
    glUseProgram(0);

    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    index_buffers_.resize(kIndexBufferPool);
    glGenBuffers(static_cast<GLsizei>(index_buffers_.size()), index_buffers_.data());
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(GlVertex), reinterpret_cast<void *>(offsetof(GlVertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(GlVertex), reinterpret_cast<void *>(offsetof(GlVertex, u)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GlVertex), reinterpret_cast<void *>(offsetof(GlVertex, r)));
    glGenVertexArrays(1, &present_vao_);
    glGenTextures(1, &scratch_texture_);
    // Bound whenever a draw samples nothing, so no framebuffer texture stays
    // bound to the sampler (WebGL rejects that as a feedback loop).
    // Samplers for every filter/wrap combination: index = linear | clamp_u << 1 | clamp_v << 2.
    glGenSamplers(8, samplers_);
    for (int i = 0; i < 8; ++i) {
        const GLint filter = (i & 1) != 0 ? GL_LINEAR : GL_NEAREST;
        glSamplerParameteri(samplers_[i], GL_TEXTURE_MIN_FILTER, filter);
        glSamplerParameteri(samplers_[i], GL_TEXTURE_MAG_FILTER, filter);
        glSamplerParameteri(samplers_[i], GL_TEXTURE_WRAP_S, (i & 2) != 0 ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        glSamplerParameteri(samplers_[i], GL_TEXTURE_WRAP_T, (i & 4) != 0 ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    }
    glGenTextures(1, &blank_texture_);
    glBindTexture(GL_TEXTURE_2D, blank_texture_);
    const std::uint32_t white = 0xFFFFFFFFu;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
    glGenTextures(1, &staging_texture_);
    glBindTexture(GL_TEXTURE_2D, staging_texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kStagingWidth, kStagingHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &staging_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, staging_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, staging_texture_, 0);
    glGenFramebuffers(1, &stencil_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, stencil_fbo_);
    const GLenum none = GL_NONE;
    glDrawBuffers(1, &none);
    glReadBuffer(GL_NONE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindVertexArray(0);
    return glGetError() == GL_NO_ERROR;
}

int GeGl::max_render_scale() const {
    GLint max_texture = 0, max_renderbuffer = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture);
    glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &max_renderbuffer);
    const GLint limit = std::min(max_texture, max_renderbuffer);
    int scale = 4;
    while (scale > 1 && static_cast<GLint>(kMaxSurfaceStride) * scale > limit) --scale;
    return scale;
}

int GeGl::set_render_scale(int scale) {
    scale = std::clamp(scale, 1, max_render_scale());
    if (scale == scale_) return scale_;
    flush();
    for (auto &s : surfaces_) {
        if (!s.alive) continue;
        download(s);
        glDeleteFramebuffers(1, &s.fbo);
        glDeleteTextures(1, &s.color);
        glDeleteRenderbuffers(1, &s.depth);
    }
    surfaces_.clear();
    current_ = nullptr;
    scale_ = scale;
    reset_cache();
    return scale_;
}

void GeGl::blit(GLuint from, const GLint src[4], GLuint to, const GLint dst[4], GLbitfield mask) {
    ++passes_.blits;
    set_enabled(GL_SCISSOR_TEST, cache_.scissor_test, false);
    if (cache_.color_mask != 15) {
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        cache_.color_mask = 15;
    }
    if ((mask & GL_DEPTH_BUFFER_BIT) != 0u && cache_.depth_mask != 1) {
        glDepthMask(GL_TRUE);
        cache_.depth_mask = 1;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, from);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, to);
    glBlitFramebuffer(src[0], src[1], src[2], src[3], dst[0], dst[1], dst[2], dst[3], mask, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    cache_.fbo = 0u;
}

// ---------------------------------------------------------------------------
// Surfaces

bool GeGl::locate(const Surface &s, std::uint32_t offset, int &ox, int &oy) const {
    if (!s.alive || offset < s.offset) return false;
    const std::uint32_t diff = offset - s.offset, bpp = bpp_of(s.format);
    if (diff % bpp != 0u) return false;
    const std::uint32_t row = diff / row_bytes(s);
    if (row >= s.height) return false;
    oy = static_cast<int>(row);
    ox = static_cast<int>((diff % row_bytes(s)) / bpp);
    return true;
}

GeGl::Surface *GeGl::find_surface(std::uint32_t offset, std::uint32_t stride, std::uint32_t format, int &ox, int &oy) {
    for (auto &s : surfaces_)
        if (s.alive && s.stride == stride && s.format == format && locate(s, offset, ox, oy)) return &s;
    return nullptr;
}

bool GeGl::is_gpu_target(std::uint32_t address, std::uint32_t format, std::uint32_t buf_width) const {
    const std::uint32_t offset = vram_offset(address);
    if (offset == kNotVram) return false;
    for (const auto &s : surfaces_) {
        int ox = 0, oy = 0;
        if (!s.alive || s.stride != buf_width || s.format != format || !locate(s, offset, ox, oy)) continue;
        if (static_cast<std::size_t>(oy) < s.rows.size()) {
            const auto &row = s.rows[static_cast<std::size_t>(oy)];
            if (row.cx0 <= ox && ox <= row.cx1) return true;
        }
    }
    return false;
}

void GeGl::allocate(Surface &s, std::uint32_t height) {
    s.height = std::clamp(height, 1u, kMaxSurfaceHeight);
    s.rows.resize(s.height);
    glGenTextures(1, &s.color);
    glBindTexture(GL_TEXTURE_2D, s.color);
    const auto width = static_cast<GLsizei>(s.stride) * scale_, height_px = static_cast<GLsizei>(s.height) * scale_;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height_px, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenRenderbuffers(1, &s.depth);
    glBindRenderbuffer(GL_RENDERBUFFER, s.depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height_px);
    glGenFramebuffers(1, &s.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s.depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        std::cerr << "[gl] incomplete framebuffer at VRAM 0x" << std::hex << s.offset << std::dec << "\n";
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFFu);
    glClearDepthf(0.0f);
    glClearStencil(0);
    glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    s.stencil_bits = 0u;
    s.alpha_bits = 0u;
    reset_cache();
}

// Reallocates `s` taller, keeping its pixels with a GPU blit.
void GeGl::grow(Surface &s, std::uint32_t height) {
    height = std::min(height, kMaxSurfaceHeight);
    if (height <= s.height) return;
    Surface old = s;
    allocate(s, height);
    const GLint area[4] = {0, 0, static_cast<GLint>(s.stride) * scale_, static_cast<GLint>(old.height) * scale_};
    blit(old.fbo, area, s.fbo, area, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDeleteFramebuffers(1, &old.fbo);
    glDeleteTextures(1, &old.color);
    glDeleteRenderbuffers(1, &old.depth);
    upload_rect(s, Rect{0, static_cast<int>(old.height), static_cast<int>(s.stride) - 1, static_cast<int>(s.height) - 1});
    reset_cache();
}

// Moves `from` (same layout, starting inside `into`) into `into` with a GPU blit.
void GeGl::absorb(Surface &into, Surface &from) {
    int ox = 0, oy = 0;
    if (!locate(into, from.offset, ox, oy)) return;
    apply_pending_uploads(into);
    apply_pending_uploads(from);
    for (Surface *each : {&into, &from}) {
        resolve_alpha(*each, whole(*each));
        resolve_stencil(*each);
    }
    if (static_cast<std::uint32_t>(oy) + from.height > into.height) grow(into, static_cast<std::uint32_t>(oy) + from.height);
    const int w = std::min(static_cast<int>(from.stride), static_cast<int>(into.stride) - ox);
    const int h = std::min(static_cast<int>(from.height), static_cast<int>(into.height) - oy);
    if (w > 0 && h > 0) {
        const GLint src[4] = {0, 0, w * scale_, h * scale_};
        const GLint dst[4] = {ox * scale_, oy * scale_, (ox + w) * scale_, (oy + h) * scale_};
        blit(from.fbo, src, into.fbo, dst, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
    // Carry the drawn and dirty extents over row by row.
    if (into.rows.size() < into.height) into.rows.resize(into.height);
    for (std::size_t y = 0; y < from.rows.size(); ++y) {
        const auto &src = from.rows[y];
        const std::size_t ty = y + static_cast<std::size_t>(oy);
        if (ty >= into.rows.size()) break;
        auto &dst = into.rows[ty];
        auto merge = [ox](int &d0, int &d1, int s0, int s1) {
            if (s1 < s0) return;
            if (d1 < d0) { d0 = s0 + ox; d1 = s1 + ox; }
            else { d0 = std::min(d0, s0 + ox); d1 = std::max(d1, s1 + ox); }
        };
        merge(dst.cx0, dst.cx1, src.cx0, src.cx1);
        merge(dst.dx0, dst.dx1, src.dx0, src.dx1);
    }
    recompute_bounds(into);
    into.stencil_bits |= from.stencil_bits;
    into.alpha_bits |= from.alpha_bits;
    from.alive = false;
    glDeleteFramebuffers(1, &from.fbo);
    glDeleteTextures(1, &from.color);
    glDeleteRenderbuffers(1, &from.depth);
    if (current_ == &from) current_ = nullptr;
    reset_cache();
}

GeGl::Surface &GeGl::surface_for_draw(std::uint32_t offset, std::uint32_t stride, std::uint32_t format,
                                      std::uint32_t rows, int &ox, int &oy) {
    rows = std::clamp(rows, 1u, kMaxSurfaceHeight);
    stride = std::max(stride, 1u);
    const std::uint32_t bpp = bpp_of(format);
    for (auto &s : surfaces_) {
        if (!s.alive || s.stride != stride || s.format != format || offset < s.offset) continue;
        const std::uint32_t diff = offset - s.offset;
        if (diff % bpp != 0u || diff / row_bytes(s) >= s.height) continue;
        oy = static_cast<int>(diff / row_bytes(s));
        ox = static_cast<int>((diff % row_bytes(s)) / bpp);
        if (static_cast<std::uint32_t>(oy) + rows > s.height) grow(s, static_cast<std::uint32_t>(oy) + rows);
        return s;
    }
    surfaces_.emplace_back();
    Surface &n = surfaces_.back();
    n.offset = offset;
    n.stride = stride;
    n.format = format;
    allocate(n, rows);
    upload_rect(n, Rect{0, 0, static_cast<int>(n.stride) - 1, static_cast<int>(n.height) - 1});
    // Surfaces of the same layout that start inside the new one become part of it.
    for (auto &s : surfaces_) {
        if (&s == &n || !s.alive || s.stride != stride || s.format != format || s.offset <= offset) continue;
        const std::uint32_t diff = s.offset - offset;
        if (diff % bpp != 0u || diff / row_bytes(n) >= kMaxSurfaceHeight) continue;
        if (diff / row_bytes(n) >= n.height && diff / row_bytes(n) >= rows) continue;
        absorb(n, s);
    }
    ox = 0;
    oy = 0;
    return n;
}

void GeGl::apply_pending_uploads(Surface &s) {
    if (s.pending_uploads.empty()) return;
    std::vector<Rect> rects;
    rects.swap(s.pending_uploads);
    for (const Rect &r : rects) upload_rect(s, r);
}

void GeGl::download(Surface &s) {
    apply_pending_uploads(s); // VRAM is newer there; keep it
    resolve_alpha(s, s.gpu_dirty);
    std::uint8_t *vram = memory_.raw_pointer(0x04000000u, kVramSize);
    const Rect r = s.gpu_dirty.clip(Rect{0, 0, static_cast<int>(s.stride) - 1, static_cast<int>(s.height) - 1});
    s.gpu_dirty = Rect{};
    std::vector<std::pair<int, int>> spans(s.rows.size(), {0, -1});
    for (std::size_t y = 0; y < s.rows.size(); ++y) {
        spans[y] = {s.rows[y].dx0, s.rows[y].dx1};
        s.rows[y].dx0 = 0;
        s.rows[y].dx1 = -1;
    }
    if (vram == nullptr || r.empty()) return;
    ++downloads_;
    const auto x0 = static_cast<std::uint32_t>(r.x0), y0 = static_cast<std::uint32_t>(r.y0);
    const auto w = static_cast<std::uint32_t>(r.x1 - r.x0 + 1), h = static_cast<std::uint32_t>(r.y1 - r.y0 + 1);
    pixels_.resize(static_cast<std::size_t>(w) * h * 4u);
    if (scale_ == 1) {
        glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
        glReadPixels(static_cast<GLint>(x0), static_cast<GLint>(y0), static_cast<GLsizei>(w), static_cast<GLsizei>(h), GL_RGBA,
                     GL_UNSIGNED_BYTE, pixels_.data());
    } else {
        // Shrink to native size first; VRAM holds PSP pixels.
        const auto sw = std::min(static_cast<GLint>(w), kStagingWidth), sh = std::min(static_cast<GLint>(h), kStagingHeight);
        const GLint src[4] = {static_cast<GLint>(x0) * scale_, static_cast<GLint>(y0) * scale_,
                              (static_cast<GLint>(x0) + sw) * scale_, (static_cast<GLint>(y0) + sh) * scale_};
        const GLint dst[4] = {0, 0, sw, sh};
        blit(s.fbo, src, staging_fbo_, dst, GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, staging_fbo_);
        glReadPixels(0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h), GL_RGBA, GL_UNSIGNED_BYTE, pixels_.data());
    }
    const std::uint32_t bpp = bpp_of(s.format);
    for (std::uint32_t y = 0; y < h; ++y) {
        const auto &span = spans[y0 + y];
        for (std::uint32_t x = 0; x < w; ++x) {
            const auto px = static_cast<int>(x0 + x);
            if (px < span.first || px > span.second) continue; // only pixels the GPU drew
            const std::uint32_t at = s.offset + ((y0 + y) * s.stride + x0 + x) * bpp;
            if (at + bpp > kVramSize) break;
            std::uint32_t rgba;
            std::memcpy(&rgba, &pixels_[(static_cast<std::size_t>(y) * w + x) * 4u], 4u);
            if (bpp == 4u) {
                std::memcpy(vram + at, &rgba, 4u);
            } else {
                const std::uint32_t packed = from_rgba(rgba, s.format);
                vram[at] = static_cast<std::uint8_t>(packed);
                vram[at + 1u] = static_cast<std::uint8_t>(packed >> 8u);
            }
        }
    }
    if (vram_write_hook_) vram_write_hook_(0x04000000u + s.offset + y0 * s.stride * bpp, h * s.stride * bpp);
    cache_.fbo = ~0u;
}

void GeGl::upload_rect(Surface &s, const Rect &rect) {
    const std::uint8_t *vram = memory_.raw_pointer(0x04000000u, kVramSize);
    const Rect r = rect.clip(Rect{0, 0, static_cast<int>(s.stride) - 1, static_cast<int>(s.height) - 1});
    if (vram == nullptr || r.empty()) return;
    alpha_will_change(s, r);
    ++uploads_;
    upload_pixels_ += static_cast<std::uint64_t>(r.x1 - r.x0 + 1) * static_cast<std::uint64_t>(r.y1 - r.y0 + 1);
    const auto x0 = static_cast<std::uint32_t>(r.x0), y0 = static_cast<std::uint32_t>(r.y0);
    const auto w = static_cast<std::uint32_t>(r.x1 - r.x0 + 1), h = static_cast<std::uint32_t>(r.y1 - r.y0 + 1);
    const std::uint32_t bpp = bpp_of(s.format);
    pixels_.assign(static_cast<std::size_t>(w) * h * 4u, 0u);
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::uint32_t row_at = s.offset + ((y0 + y) * s.stride + x0) * bpp;
        if (bpp == 4u && row_at + w * 4u <= kVramSize) {
            std::memcpy(&pixels_[static_cast<std::size_t>(y) * w * 4u], vram + row_at, w * 4u);
            continue;
        }
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint32_t at = s.offset + ((y0 + y) * s.stride + x0 + x) * bpp;
            if (at + bpp > kVramSize) break;
            std::uint32_t rgba;
            if (bpp == 4u) std::memcpy(&rgba, vram + at, 4u);
            else rgba = to_rgba(static_cast<std::uint32_t>(vram[at]) | (static_cast<std::uint32_t>(vram[at + 1u]) << 8u), s.format);
            std::memcpy(&pixels_[(static_cast<std::size_t>(y) * w + x) * 4u], &rgba, 4u);
        }
    }
    if (s.format != 0u) {
        std::uint32_t bits = 0u;
        for (std::size_t i = 3; i < pixels_.size(); i += 4u) bits |= pixels_[i];
        s.alpha_bits |= bits;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (scale_ == 1) {
        glBindTexture(GL_TEXTURE_2D, s.color);
        glTexSubImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(x0), static_cast<GLint>(y0), static_cast<GLsizei>(w),
                        static_cast<GLsizei>(h), GL_RGBA, GL_UNSIGNED_BYTE, pixels_.data());
    } else {
        // Upload at native size, then stretch into the surface.
        const auto sw = std::min(static_cast<GLint>(w), kStagingWidth), sh = std::min(static_cast<GLint>(h), kStagingHeight);
        glBindTexture(GL_TEXTURE_2D, staging_texture_);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(w));
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, pixels_.data());
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        const GLint src[4] = {0, 0, sw, sh};
        const GLint dst[4] = {static_cast<GLint>(x0) * scale_, static_cast<GLint>(y0) * scale_,
                              (static_cast<GLint>(x0) + sw) * scale_, (static_cast<GLint>(y0) + sh) * scale_};
        blit(staging_fbo_, src, s.fbo, dst, GL_COLOR_BUFFER_BIT);
    }
    cache_.texture = ~0u;
}

GeGl::Rect GeGl::overlap(const Surface &s, const TransferRect &rect) const {
    Rect out;
    const std::uint32_t base = vram_offset(rect.base);
    if (!s.alive || base == kNotVram || rect.width == 0u || rect.height == 0u) return out;
    const std::uint32_t bpp = bpp_of(s.format), rb = row_bytes(s), end = surface_end(s);
    const std::uint32_t span_first = base + (rect.y * rect.stride + rect.x) * rect.bpp;
    const std::uint32_t span_last = base + ((rect.y + rect.height - 1u) * rect.stride + rect.x + rect.width) * rect.bpp;
    if (span_last <= s.offset || span_first >= end) return out; // quick reject
    for (std::uint32_t row = 0; row < rect.height; ++row) {
        const std::uint32_t start = base + ((rect.y + row) * rect.stride + rect.x) * rect.bpp;
        const std::uint32_t stop = start + rect.width * rect.bpp; // exclusive
        if (stop <= s.offset || start >= end) continue;
        const std::uint32_t from = std::max(start, s.offset) - s.offset, to = std::min(stop, end) - s.offset;
        const auto y0 = static_cast<int>(from / rb), y1 = static_cast<int>((to - 1u) / rb);
        if (y0 == y1) out.add(static_cast<int>((from % rb) / bpp), y0, static_cast<int>(((to - 1u) % rb) / bpp), y1);
        else out.add(0, y0, static_cast<int>(s.stride) - 1, y1);
    }
    return out;
}

bool GeGl::reinterpret(Surface &into, const Surface &from, const Rect &hit) {
    const bool into_32 = into.format == 3u, from_32 = from.format == 3u;
    if (into_32 == from_32 || row_bytes(into) != row_bytes(from) || hit.empty()) return false;
    const auto delta = static_cast<int>(static_cast<std::int64_t>(into.offset) - static_cast<std::int64_t>(from.offset));
    if (delta % 2 != 0) return false;
    apply_pending_uploads(into);
    apply_pending_uploads(const_cast<Surface &>(from));
    resolve_alpha(const_cast<Surface &>(from), whole(from));
    alpha_will_change(into, hit);
    into.alpha_bits = 0xFFu;
    ++passes_.reinterprets;
    if (frame_ == g_pass_frame)
        std::cerr << "[pass] reinterpret into=0x" << std::hex << into.offset << "/f" << into.format << " from=0x" << from.offset << "/f" << from.format
                  << std::dec << " " << hit.x0 << "," << hit.y0 << ".." << hit.x1 << "," << hit.y1 << "\n";
    passes_.pixels += static_cast<std::uint64_t>((hit.x1 - hit.x0 + 1) * (hit.y1 - hit.y0 + 1) * scale_ * scale_);
    glBindFramebuffer(GL_FRAMEBUFFER, into.fbo);
    glDisable(GL_STENCIL_TEST);
    glViewport(0, 0, static_cast<GLsizei>(into.stride) * scale_, static_cast<GLsizei>(into.height) * scale_);
    glEnable(GL_SCISSOR_TEST);
    glScissor(hit.x0 * scale_, hit.y0 * scale_, (hit.x1 - hit.x0 + 1) * scale_, (hit.y1 - hit.y0 + 1) * scale_);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(reinterpret_program_);
    glBindVertexArray(present_vao_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, from.color);
    glBindSampler(0, samplers_[0]);
    glUniform1i(u_ri_src_, 0);
    glUniform1i(u_ri_mode_, into_32 ? 1 : 0);
    glUniform1i(u_ri_format_, static_cast<GLint>(into_32 ? from.format : into.format));
    glUniform1i(u_ri_delta_, delta);
    glUniform1i(u_ri_row_bytes_, static_cast<GLint>(row_bytes(into)));
    glUniform2i(u_ri_src_size_, static_cast<GLint>(from.stride), static_cast<GLint>(from.height));
    glUniform1i(u_ri_scale_, scale_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    reset_cache();
    return true;
}

void GeGl::alpha_will_change(Surface &s, const Rect &r) {
    if (s.format == 0u) return; // RGB565 has neither alpha nor stencil
    // Keep the two stale areas apart: settle the alpha first if they would touch.
    Rect grown = s.stencil_stale;
    grown.add(r);
    if (grown.intersects(s.alpha_stale)) resolve_alpha(s, whole(s));
    s.stencil_stale.add(r);
}

void GeGl::resolve_alpha(Surface &s, const Rect &region, std::uint32_t bits) {
    if (s.format == 0u) {
        s.alpha_stale = Rect{};
        return;
    }
    const Rect r = s.alpha_stale.clip(region);
    if (r.empty()) return;
    const bool complete = (bits & 0xFFu) == 0xFFu;
    if (complete && r.x0 == s.alpha_stale.x0 && r.y0 == s.alpha_stale.y0 && r.x1 == s.alpha_stale.x1 &&
        r.y1 == s.alpha_stale.y1)
        s.alpha_stale = Rect{};
    if (s.stale_value >= 0) {
        // One known value throughout: a single clear sets it.
        ++passes_.alpha_resolves;
        passes_.pixels += static_cast<std::uint64_t>((r.x1 - r.x0 + 1) * (r.y1 - r.y0 + 1) * scale_ * scale_);
        s.alpha_bits |= static_cast<std::uint32_t>(s.stale_value);
        glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
        glEnable(GL_SCISSOR_TEST);
        glScissor(r.x0 * scale_, r.y0 * scale_, (r.x1 - r.x0 + 1) * scale_, (r.y1 - r.y0 + 1) * scale_);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
        glClearColor(0.0f, 0.0f, 0.0f, static_cast<float>(s.stale_value) / 255.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        if (r.x0 == s.alpha_stale.x0 && r.y0 == s.alpha_stale.y0 && r.x1 == s.alpha_stale.x1 && r.y1 == s.alpha_stale.y1) {
            s.alpha_stale = Rect{};
            s.stale_value = -1;
        }
        reset_cache();
        return;
    }
    s.alpha_bits |= s.stencil_bits;
    const std::uint32_t wanted = s.stencil_bits & bits;
    ++passes_.alpha_resolves;
    if (frame_ == g_pass_frame)
        std::cerr << "[pass] alpha bits=0x" << std::hex << wanted << " vram=0x" << s.offset << std::dec << " " << r.x0 << ","
                  << r.y0 << ".." << r.x1 << "," << r.y1 << "\n";
    passes_.pixels += static_cast<std::uint64_t>(1 + std::popcount(wanted)) *
                      static_cast<std::uint64_t>((r.x1 - r.x0 + 1) * (r.y1 - r.y0 + 1) * scale_ * scale_);
    glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    glViewport(0, 0, static_cast<GLsizei>(s.stride) * scale_, static_cast<GLsizei>(s.height) * scale_);
    glEnable(GL_SCISSOR_TEST);
    glScissor(r.x0 * scale_, r.y0 * scale_, (r.x1 - r.x0 + 1) * scale_, (r.y1 - r.y0 + 1) * scale_);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0u);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glUseProgram(alpha_program_);
    glBindVertexArray(present_vao_);
    for (GLuint bit = 0; bit < 8u; ++bit) {
        if ((wanted & (1u << bit)) == 0u) continue;
        glStencilFunc(GL_EQUAL, static_cast<GLint>(1u << bit), 1u << bit);
        glUniform1f(u_al_value_, static_cast<float>(1u << bit) / 255.0f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    reset_cache();
}

void GeGl::resolve_stencil(Surface &s) {
    const Rect r = s.stencil_stale.clip(whole(s));
    s.stencil_stale = Rect{};
    if (r.empty() || s.format == 0u) return;
    s.stencil_bits |= s.alpha_bits;
    ++passes_.stencil_resolves;
    if (frame_ == g_pass_frame)
        std::cerr << "[pass] stencil bits=0x" << std::hex << s.alpha_bits << " vram=0x" << s.offset << std::dec << " " << r.x0 << ","
                  << r.y0 << ".." << r.x1 << "," << r.y1 << "\n";
    passes_.pixels += static_cast<std::uint64_t>(1 + std::popcount(s.alpha_bits)) *
                      static_cast<std::uint64_t>((r.x1 - r.x0 + 1) * (r.y1 - r.y0 + 1) * scale_ * scale_);
    glBindFramebuffer(GL_FRAMEBUFFER, stencil_fbo_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s.depth);
    glViewport(0, 0, static_cast<GLsizei>(s.stride) * scale_, static_cast<GLsizei>(s.height) * scale_);
    glEnable(GL_SCISSOR_TEST);
    glScissor(r.x0 * scale_, r.y0 * scale_, (r.x1 - r.x0 + 1) * scale_, (r.y1 - r.y0 + 1) * scale_);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDepthMask(GL_FALSE);
    glStencilMask(0xFFu);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, 0xFF, 0xFFu);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glUseProgram(stencil_program_);
    glBindVertexArray(present_vao_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s.color);
    glBindSampler(0, samplers_[0]);
    glUniform1i(u_st_src_, 0);
    for (GLuint bit = 0; bit < 8u; ++bit) {
        if ((s.alpha_bits & (1u << bit)) == 0u) continue;
        glStencilMask(1u << bit);
        glUniform1i(u_st_bit_, static_cast<GLint>(bit));
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
    reset_cache();
}

void GeGl::sync_overlaps(Surface &s) {
    for (auto &other : surfaces_) {
        if (&other == &s || !other.alive || other.content.empty() || other.last_draw == 0u) continue;
        auto synced = std::find_if(s.synced.begin(), s.synced.end(), [&](const auto &e) { return e.first == &other; });
        if (synced != s.synced.end() && synced->second >= other.last_draw) continue;
        const std::uint32_t bpp = bpp_of(other.format), rb = row_bytes(other);
        const std::uint32_t begin = other.offset + static_cast<std::uint32_t>(other.content.y0) * rb;
        const std::uint32_t end = other.offset + static_cast<std::uint32_t>(other.content.y1 + 1) * rb;
        if (!(begin < surface_end(s) && s.offset < end)) continue;
        const TransferRect drawn{0x04000000u + other.offset, other.stride, static_cast<std::uint32_t>(other.content.x0),
                                 static_cast<std::uint32_t>(other.content.y0),
                                 static_cast<std::uint32_t>(other.content.x1 - other.content.x0 + 1),
                                 static_cast<std::uint32_t>(other.content.y1 - other.content.y0 + 1), bpp};
        const Rect hit = overlap(s, drawn);
        if (synced == s.synced.end()) {
            s.synced.emplace_back(&other, 0u);
            synced = s.synced.end() - 1;
        }
        synced->second = other.last_draw;
        if (hit.empty()) continue;
        // Earlier batches are already drawn: surfaces are synced while applying state.
        if (!reinterpret(s, other, hit)) {
            download(other);
            upload_rect(s, hit);
        }
        mark_drawn(s, hit);
    }
}

bool GeGl::batch_uses(const Surface &s) const {
    if (batch_.empty() || !state_valid_) return false;
    int ox = 0, oy = 0;
    if (s.stride == state_.fb_stride && s.format == state_.fb_format && locate(s, state_.fb_offset, ox, oy)) return true;
    return state_.tex_from_target && locate(s, vram_offset(state_.tex_address), ox, oy);
}

bool GeGl::prepare_vram_read(std::uint32_t address, std::uint32_t bytes) {
    const std::uint32_t offset = vram_offset(address);
    if (offset == kNotVram) return false;
    bool downloaded = false;
    for (auto &s : surfaces_) {
        if (!s.alive || s.gpu_dirty.empty()) continue;
        const std::uint32_t rb = row_bytes(s);
        const std::uint32_t begin = s.offset + static_cast<std::uint32_t>(s.gpu_dirty.y0) * rb;
        const std::uint32_t end = s.offset + static_cast<std::uint32_t>(s.gpu_dirty.y1 + 1) * rb;
        if (offset < end && begin < offset + bytes) {
            if (batch_uses(s)) flush();
            download(s);
            downloaded = true;
        }
    }
    return downloaded;
}

// ---------------------------------------------------------------------------
// Drawing

void GeGl::set_state(const GlDrawState &state) {
    if (state_valid_ && state == state_) return;
    flush();
    state_ = state;
    state_valid_ = true;
}

void GeGl::triangle(const GlVertex &a, const GlVertex &b, const GlVertex &c) {
    const auto base = static_cast<std::uint32_t>(batch_.size());
    batch_.push_back(a);
    batch_.push_back(b);
    batch_.push_back(c);
    indices_.push_back(base);
    indices_.push_back(base + 1u);
    indices_.push_back(base + 2u);
}

// `source` is the surface a render-to-texture draw samples, already brought up
// to date by apply_state.
GLuint GeGl::texture_for_state(Surface *source, int ox, int oy) {
    const GlDrawState &s = state_;
    if (s.tex_from_target) {
        if (source != nullptr) {
            set_uniform(u_tex_size_, static_cast<float>(source->stride), static_cast<float>(source->height), 0.0f, 2);
            set_uniform(u_tex_origin_, static_cast<float>(ox), static_cast<float>(oy), 0.0f, 2);
            set_uniform(u_tex_window_, static_cast<float>(s.tex_width), static_cast<float>(s.tex_height), 0.0f, 2);
            set_uniform(u_tex_clamp_, s.clamp_u ? 1.0f : 0.0f, s.clamp_v ? 1.0f : 0.0f, 0.0f, 2);
            set_uniform_int(u_tex_opaque_, s.tex_format == 0u ? 1 : 0);
            if (source != current_) return source->color;
            ++passes_.feedback_copies;
            // Sampling the surface being drawn: read from a snapshot of the sampled part.
            bind_texture0(scratch_texture_);
            const auto width = static_cast<GLsizei>(source->stride) * scale_;
            const auto height = static_cast<GLsizei>(source->height) * scale_;
            if (scratch_width_ != width || scratch_height_ != height) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                scratch_width_ = width;
                scratch_height_ = height;
            }
            const Rect window = Rect{ox, oy, ox + static_cast<int>(s.tex_width) - 1, oy + static_cast<int>(s.tex_height) - 1}
                                    .clip(whole(*source));
            if (!window.empty()) {
                passes_.pixels += static_cast<std::uint64_t>((window.x1 - window.x0 + 1) * (window.y1 - window.y0 + 1) * scale_ * scale_);
                if (frame_ == g_pass_frame)
                    std::cerr << "[pass] feedback vram=0x" << std::hex << source->offset << std::dec << " " << window.x0 << "," << window.y0
                              << ".." << window.x1 << "," << window.y1 << "\n";
                bind_framebuffer(source->fbo);
                glCopyTexSubImage2D(GL_TEXTURE_2D, 0, window.x0 * scale_, window.y0 * scale_, window.x0 * scale_,
                                    window.y0 * scale_, (window.x1 - window.x0 + 1) * scale_,
                                    (window.y1 - window.y0 + 1) * scale_);
            }
            return scratch_texture_;
        }
    }
    if (s.texels == nullptr) return 0u;
    if (textures_.size() > kMaxTextures) {
        for (auto &[key, texture] : textures_) glDeleteTextures(1, &texture.id);
        textures_.clear();
        cache_.texture = ~0u;
    }
    GlTexture &texture = textures_[s.texture_identity];
    if (texture.id == 0u) glGenTextures(1, &texture.id);
    if (texture.hash != s.texture_hash || texture.width != s.tex_width || texture.height != s.tex_height) {
        bind_texture0(texture.id);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(s.tex_width), static_cast<GLsizei>(s.tex_height), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, s.texels);
        texture.hash = s.texture_hash;
        texture.width = s.tex_width;
        texture.height = s.tex_height;
    }
    set_uniform(u_tex_size_, static_cast<float>(s.tex_width), static_cast<float>(s.tex_height), 0.0f, 2);
    set_uniform(u_tex_origin_, 0.0f, 0.0f, 0.0f, 2);
    set_uniform(u_tex_window_, 0.0f, 0.0f, 0.0f, 2);
    set_uniform_int(u_tex_opaque_, 0);
    return texture.id;
}

void GeGl::forget_texture(const void *identity) {
    const auto it = textures_.find(identity);
    if (it == textures_.end()) return;
    if (cache_.texture == it->second.id) cache_.texture = ~0u;
    glDeleteTextures(1, &it->second.id);
    textures_.erase(it);
}

void GeGl::apply_state() {
    const GlDrawState &s = state_;
    int ox = 0, oy = 0;
    Surface &surface = surface_for_draw(s.fb_offset, s.fb_stride, s.fb_format,
                                        static_cast<std::uint32_t>(std::max(s.sy2, 0) + 1), ox, oy);
    apply_pending_uploads(surface);
    sync_overlaps(surface);
    // Everything that may run its own GL passes (syncing a sampled surface,
    // moving stencil to alpha or back) happens before this draw's state is set.
    Surface *source = nullptr;
    int source_ox = 0, source_oy = 0;
    if (!s.clear && s.texture && s.tex_from_target) {
        source = find_surface(vram_offset(s.tex_address), s.tex_buf_width, s.tex_format, source_ox, source_oy);
        if (source != nullptr) {
            apply_pending_uploads(*source);
            if (source != &surface) sync_overlaps(*source);
            // The texture's alpha only matters if something downstream looks at it.
            const bool src_alpha_blend = s.blend && (is_src_alpha_factor(s.src_factor) || is_src_alpha_factor(s.dst_factor));
            if (s.tex_alpha && s.tex_format != 0u && (s.alpha_test || src_alpha_blend || s.tex_func == 1u)) {
                // Read only by a masked alpha test (and vertex alpha is 1, so texel alpha
                // reaches the test unchanged): only the tested bits need to be right.
                std::uint32_t bits = 0xFFu;
                if (!src_alpha_blend && s.tex_func != 1u &&
                    std::all_of(batch_.begin(), batch_.end(), [](const GlVertex &v) { return v.a == 255u; }))
                    bits = s.alpha_mask & 0xFFu;
                const Rect window{source_ox, source_oy, source_ox + static_cast<int>(s.tex_width) - 1,
                                  source_oy + static_cast<int>(s.tex_height) - 1};
                resolve_alpha(*source, window, bits);
            }
        }
    }
    const bool stencil_on = s.clear ? (s.clear_flags & 0x200u) != 0u : s.stencil;
    const Rect scissored = Rect{std::max(s.sx1, 0) + ox, std::max(s.sy1, 0) + oy, s.sx2 + ox, s.sy2 + oy}.clip(whole(surface));
    if (!s.clear && s.blend && (is_dst_alpha_factor(s.src_factor) || is_dst_alpha_factor(s.dst_factor)))
        resolve_alpha(surface, scissored);
    batch_stencil_value_ = -1;
    if (stencil_on) {
        // A rectangle that sets every pixel it covers regardless of the old
        // stencil (test always passes, ops zero/replace, all bits written)
        // makes resolving the old stencil underneath it pointless.
        bool rectangle = false;
        const Rect covered = batch_bounds(rectangle).shifted(ox, oy).clip(whole(surface));
        auto overwrites = [](std::uint32_t op) { return op == 1u || op == 2u; };
        const bool independent = !s.clear && (s.stencil_func & 7u) == 1u && overwrites(s.stencil_ops & 0xFFu) &&
                                  overwrites((s.stencil_ops >> 8u) & 0xFFu) && overwrites((s.stencil_ops >> 16u) & 0xFFu) &&
                                  (s.mask_alpha & 0xFFu) == 0u;
        batch_stencil_value_ = -1;
        if (independent && rectangle) {
            const std::uint32_t ref = (s.stencil_func >> 8u) & 0xFFu;
            auto value = [ref](std::uint32_t op) { return op == 1u ? 0 : static_cast<int>(ref); };
            const int pass_value = value((s.stencil_ops >> 16u) & 0xFFu);
            if (value(s.stencil_ops & 0xFFu) == pass_value && value((s.stencil_ops >> 8u) & 0xFFu) == pass_value)
                batch_stencil_value_ = pass_value;
        }
        Rect &stale = surface.stencil_stale;
        if (independent && rectangle && !stale.empty() && covered.x0 <= stale.x0 && covered.x1 >= stale.x1) {
            // Keep only the rows the rectangle leaves uncovered (above or below it).
            if (covered.y0 <= stale.y0 && covered.y1 >= stale.y1) stale = Rect{};
            else if (covered.y0 <= stale.y0 && covered.y1 >= stale.y0) stale.y0 = covered.y1 + 1;
            else if (covered.y1 >= stale.y1 && covered.y0 <= stale.y1) stale.y1 = covered.y0 - 1;
        }
        resolve_stencil(surface);
    }
    batch_writes_stencil_ = false;
    current_ = &surface;
    current_ox_ = ox;
    current_oy_ = oy;
    use_program(program_);
    if (cache_.vao != vao_) {
        glBindVertexArray(vao_);
        cache_.vao = vao_;
    }
    set_uniform(u_target_, static_cast<float>(surface.stride), static_cast<float>(surface.height), 0.0f, 2);
    set_uniform(u_origin_, static_cast<float>(ox), static_cast<float>(oy), 0.0f, 2);
    set_uniform_int(u_clear_, s.clear ? 1 : 0);

    GLuint texture = 0u;
    if (!s.clear && s.texture) texture = texture_for_state(source, source_ox, source_oy);

    bind_framebuffer(surface.fbo);
    const GLint viewport_w = static_cast<GLint>(surface.stride) * scale_, viewport_h = static_cast<GLint>(surface.height) * scale_;
    if (cache_.viewport_w != viewport_w || cache_.viewport_h != viewport_h) {
        glViewport(0, 0, viewport_w, viewport_h);
        cache_.viewport_w = viewport_w;
        cache_.viewport_h = viewport_h;
    }
    set_enabled(GL_SCISSOR_TEST, cache_.scissor_test, true);
    const int x1 = std::max(s.sx1, 0) + ox, y1 = std::max(s.sy1, 0) + oy;
    const GLint scissor[4] = {x1 * scale_, y1 * scale_, std::max(s.sx2 + ox - x1 + 1, 0) * scale_,
                              std::max(s.sy2 + oy - y1 + 1, 0) * scale_};
    if (!std::equal(scissor, scissor + 4, cache_.scissor)) {
        glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
        std::copy(scissor, scissor + 4, cache_.scissor);
    }
    // PSP stencil -> GL stencil.  Clears with the alpha flag set it to the clear alpha.
    set_enabled(GL_STENCIL_TEST, cache_.stencil_test, stencil_on);
    if (stencil_on) {
        GLenum func = GL_ALWAYS, fail = GL_REPLACE, zfail = GL_REPLACE, zpass = GL_REPLACE;
        GLint ref = clear_alpha_;
        GLuint mask = 0xFFu, write = 0xFFu;
        if (!s.clear) {
            func = depth_func(s.stencil_func & 7u);
            ref = static_cast<GLint>((s.stencil_func >> 8u) & 0xFFu);
            mask = (s.stencil_func >> 16u) & 0xFFu;
            fail = stencil_op(s.stencil_ops & 0xFFu);
            zfail = stencil_op((s.stencil_ops >> 8u) & 0xFFu);
            zpass = stencil_op((s.stencil_ops >> 16u) & 0xFFu);
            write = ~s.mask_alpha & 0xFFu;
            batch_writes_stencil_ = write != 0u && (fail != GL_KEEP || zfail != GL_KEEP || zpass != GL_KEEP);
            if (batch_writes_stencil_) {
                const bool arbitrary = fail == GL_INVERT || fail == GL_INCR || fail == GL_DECR || zfail == GL_INVERT ||
                                       zfail == GL_INCR || zfail == GL_DECR || zpass == GL_INVERT || zpass == GL_INCR ||
                                       zpass == GL_DECR;
                surface.stencil_bits |= (arbitrary ? 0xFFu : static_cast<std::uint32_t>(ref)) & write;
            }
        } else {
            surface.stencil_bits |= static_cast<std::uint32_t>(clear_alpha_) & 0xFFu;
            surface.alpha_bits |= static_cast<std::uint32_t>(clear_alpha_) & 0xFFu;
        }
        // Keyed on the raw PSP registers (or the clear value) the GL state came from.
        const std::uint64_t key = s.clear ? ((1ull << 63u) | static_cast<std::uint64_t>(clear_alpha_ & 0xFF))
                                          : (static_cast<std::uint64_t>(s.stencil_func) |
                                             (static_cast<std::uint64_t>(s.stencil_ops) << 24u) |
                                             (static_cast<std::uint64_t>(write) << 48u));
        if (cache_.stencil_key != key) {
            glStencilFunc(func, ref, mask);
            glStencilOp(fail, zfail, zpass);
            glStencilMask(write);
            cache_.stencil_key = key;
        }
    }
    auto color_mask = [this](bool r, bool g, bool b, bool a) {
        const int packed = (r ? 1 : 0) | (g ? 2 : 0) | (b ? 4 : 0) | (a ? 8 : 0);
        if (cache_.color_mask == packed) return;
        glColorMask(r, g, b, a);
        cache_.color_mask = packed;
    };
    auto depth = [this](bool test, GLenum func, bool write) {
        set_enabled(GL_DEPTH_TEST, cache_.depth_test, test);
        if (!test) return;
        if (cache_.depth_func != func) {
            glDepthFunc(func);
            cache_.depth_func = func;
        }
        if (cache_.depth_mask != static_cast<int>(write)) {
            glDepthMask(write ? GL_TRUE : GL_FALSE);
            cache_.depth_mask = static_cast<int>(write);
        }
    };

    if (s.clear) {
        const bool color = (s.clear_flags & 0x100u) != 0u, alpha = (s.clear_flags & 0x200u) != 0u;
        color_mask(color, color, color, alpha);
        depth((s.clear_flags & 0x400u) != 0u, GL_ALWAYS, true);
        set_enabled(GL_BLEND, cache_.blend, false);
        set_uniform_int(u_tex_enable_, 0);
        set_uniform_int(u_alpha_test_, 0);
        set_uniform_int(u_fog_enable_, 0);
        set_uniform(u_src_scale_, 1.0f, 1.0f, 1.0f, 3);
        bind_texture0(blank_texture_);
        return;
    }

    // Alpha is the stencil: written through the stencil buffer, never directly.
    color_mask((s.mask_rgb & 0x0000FFu) != 0x0000FFu, (s.mask_rgb & 0x00FF00u) != 0x00FF00u,
               (s.mask_rgb & 0xFF0000u) != 0xFF0000u, false);
    depth(s.z_test, depth_func(s.z_func), s.z_write);

    float src_scale[3] = {1.0f, 1.0f, 1.0f};
    set_enabled(GL_BLEND, cache_.blend, s.blend);
    if (s.blend) {
        bool uses_constant = false;
        const GLenum src = blend_factor(s.src_factor, true, s.fix_a, uses_constant);
        const GLenum dst = blend_factor(s.dst_factor, false, s.fix_b, uses_constant);
        if (s.src_factor >= 10u && src == GL_ONE && (s.fix_a & 0xFFFFFFu) != 0xFFFFFFu) {
            src_scale[0] = static_cast<float>(s.fix_a & 0xFFu) / 255.0f;
            src_scale[1] = static_cast<float>((s.fix_a >> 8u) & 0xFFu) / 255.0f;
            src_scale[2] = static_cast<float>((s.fix_a >> 16u) & 0xFFu) / 255.0f;
        }
        if (uses_constant) {
            const float color[4] = {static_cast<float>(s.fix_b & 0xFFu) / 255.0f,
                                    static_cast<float>((s.fix_b >> 8u) & 0xFFu) / 255.0f,
                                    static_cast<float>((s.fix_b >> 16u) & 0xFFu) / 255.0f, 1.0f};
            if (!std::equal(color, color + 4, cache_.blend_color)) {
                glBlendColor(color[0], color[1], color[2], color[3]);
                std::copy(color, color + 4, cache_.blend_color);
            }
        }
        if (cache_.blend_src != src || cache_.blend_dst != dst) {
            glBlendFunc(src, dst);
            cache_.blend_src = src;
            cache_.blend_dst = dst;
        }
        static constexpr GLenum equations[6] = {GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT,
                                                GL_MIN, GL_MAX, GL_FUNC_ADD};
        const GLenum equation = equations[std::min(s.equation, 5u)];
        if (cache_.blend_eq != equation) {
            glBlendEquation(equation);
            cache_.blend_eq = equation;
        }
    }
    set_uniform(u_src_scale_, src_scale[0], src_scale[1], src_scale[2], 3);
    set_uniform_int(u_fog_enable_, s.fog ? 1 : 0);
    if (s.fog) {
        set_uniform(u_fog_, s.fog_w, s.fog_bias, 0.0f, 2);
        set_uniform(u_fog_color_, static_cast<float>(s.fog_color & 0xFFu) / 255.0f,
                    static_cast<float>((s.fog_color >> 8u) & 0xFFu) / 255.0f,
                    static_cast<float>((s.fog_color >> 16u) & 0xFFu) / 255.0f, 3);
    }

    set_uniform_int(u_alpha_test_, s.alpha_test ? 1 : 0);
    if (s.alpha_test) {
        set_uniform_int(u_alpha_func_, static_cast<int>(s.alpha_func));
        set_uniform_int(u_alpha_ref_, static_cast<int>(s.alpha_ref));
        set_uniform_int(u_alpha_mask_, static_cast<int>(s.alpha_mask));
    }

    set_uniform_int(u_tex_enable_, texture != 0u ? 1 : 0);
    if (texture == 0u) {
        bind_texture0(blank_texture_);
        return;
    }
    bind_texture0(texture);
    const bool clamp_u = s.clamp_u || s.tex_from_target, clamp_v = s.clamp_v || s.tex_from_target;
    const GLuint sampler = samplers_[(s.linear ? 1 : 0) | (clamp_u ? 2 : 0) | (clamp_v ? 4 : 0)];
    if (cache_.sampler != sampler) {
        glBindSampler(0, sampler);
        cache_.sampler = sampler;
    }
    set_uniform_int(u_tex_func_, static_cast<int>(s.tex_func));
    set_uniform_int(u_tex_alpha_, s.tex_alpha ? 1 : 0);
    set_uniform_int(u_tex_double_, s.tex_double ? 1 : 0);
    if (s.tex_func == 2u)
        set_uniform(u_env_, static_cast<float>(s.env & 0xFFu) / 255.0f, static_cast<float>((s.env >> 8u) & 0xFFu) / 255.0f,
                    static_cast<float>((s.env >> 16u) & 0xFFu) / 255.0f, 3);
}

GeGl::Rect GeGl::batch_bounds(bool &rectangle) const {
    float min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f;
    for (const auto &v : batch_) {
        min_x = std::min(min_x, v.x);
        max_x = std::max(max_x, v.x);
        min_y = std::min(min_y, v.y);
        max_y = std::max(max_y, v.y);
    }
    rectangle = batch_.size() == 6u && std::all_of(batch_.begin(), batch_.end(), [&](const GlVertex &v) {
        return (v.x == min_x || v.x == max_x) && (v.y == min_y || v.y == max_y);
    });
    // Pixels whose centres a primitive can cover: floor(min) .. ceil(max) - 1.
    const int x0 = std::max(static_cast<int>(std::floor(std::max(min_x, -4096.0f))), state_.sx1);
    const int x1 = std::min(static_cast<int>(std::ceil(std::min(max_x, 4096.0f))) - 1, state_.sx2);
    const int y0 = std::max(static_cast<int>(std::floor(std::max(min_y, -4096.0f))), state_.sy1);
    const int y1 = std::min(static_cast<int>(std::ceil(std::min(max_y, 4096.0f))) - 1, state_.sy2);
    return Rect{x0, y0, x1, y1};
}

void GeGl::flush() {
    if (batch_.empty() || indices_.empty() || !state_valid_) {
        batch_.clear();
        indices_.clear();
        return;
    }
    static const bool timed = std::getenv("PSPWEB_FRAME_PROFILE") != nullptr;
    const double started = timed ? clock_ms() : 0.0;
    struct FlushTimer {
        double &total;
        double start;
        bool on;
        ~FlushTimer() { if (on) total += clock_ms() - start; }
    } flush_timer{flush_ms_, started, timed};
    clear_alpha_ = batch_.front().a;
    apply_state();
    if (current_ != nullptr) {
        bool rectangle = false;
        const Rect drawn = batch_bounds(rectangle).shifted(current_ox_, current_oy_).clip(
            Rect{0, 0, static_cast<int>(current_->stride) - 1, static_cast<int>(current_->height) - 1});
        mark_drawn(*current_, drawn);
        if (batch_writes_stencil_) {
            Rect &stale = current_->alpha_stale;
            const bool replaces_all = stale.empty() || (drawn.x0 <= stale.x0 && drawn.y0 <= stale.y0 &&
                                                         drawn.x1 >= stale.x1 && drawn.y1 >= stale.y1);
            if (batch_stencil_value_ >= 0 && replaces_all) {
                stale = drawn;
                current_->stale_value = batch_stencil_value_;
            } else {
                stale.add(drawn);
                current_->stale_value = -1;
            }
        }
        current_->last_draw = ++draw_sequence_;
    }
    // Each draw re-specifies the vertex buffer (orphaning) and gets an index
    // buffer of its own from a rotating pool.  Writing into a buffer the GPU may
    // still be reading makes mobile drivers stall or copy it, and some browsers
    // re-validate a whole index buffer after any change to it.
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(batch_.size() * sizeof(GlVertex)), batch_.data(), GL_STREAM_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, index_buffers_[next_index_buffer_]); // vao_ is bound by apply_state
    next_index_buffer_ = (next_index_buffer_ + 1u) % index_buffers_.size();
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indices_.size() * sizeof(std::uint32_t)), indices_.data(),
                 GL_STREAM_DRAW);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(indices_.size()), GL_UNSIGNED_INT, nullptr);
    indices_.clear();
    ++draw_calls_;
    batch_.clear();
}

// ---------------------------------------------------------------------------
// Block transfers and display

void GeGl::before_transfer(const TransferRect &src, const TransferRect &) {
    // Pixels only the GPU has must reach VRAM before the copy reads them.
    // Destinations need no download: the copy is newer than whatever is there.
    for (auto &s : surfaces_) {
        if (!s.alive || s.gpu_dirty.empty()) continue;
        bool hit = false;
        for_each_segment(s, src, [&](int row, int x0, int x1) {
            if (hit || static_cast<std::size_t>(row) >= s.rows.size()) return;
            const auto &span = s.rows[static_cast<std::size_t>(row)];
            hit = span.dx0 <= span.dx1 && x0 <= span.dx1 && span.dx0 <= x1;
        });
        if (hit) {
            flush();
            download(s);
        }
    }
}

void GeGl::after_transfer(const TransferRect &dst) {
    // Only pixels inside what the GPU has drawn matter: copies elsewhere (for
    // example textures packed into the unused part of a framebuffer stride) are
    // read from VRAM directly.  A copy covering a row's whole drawn extent
    // retires that row: VRAM is the truth there again and nothing is uploaded.
    for (auto &s : surfaces_) {
        if (!s.alive || s.content.empty()) continue;
        bool flushed = false, retired = false;
        Rect partial;
        for_each_segment(s, dst, [&](int row, int x0, int x1) {
            if (static_cast<std::size_t>(row) >= s.rows.size()) return;
            auto &span = s.rows[static_cast<std::size_t>(row)];
            if (span.cx1 < span.cx0 || x1 < span.cx0 || span.cx1 < x0) return;
            if (!flushed && batch_uses(s)) flush(); // draws recorded before the copy must not see it
            flushed = true;
            if (x0 <= span.cx0 && span.cx1 <= x1) {
                span = Surface::RowSpan{};
                retired = true;
                return;
            }
            partial.add(std::max(x0, span.cx0), row, std::min(x1, span.cx1), row);
        });
        if (retired) recompute_bounds(s);
        if (!partial.empty()) {
            // One rectangle per copy keeps the number of GL uploads low.
            if (!s.pending_uploads.empty()) {
                Rect &last = s.pending_uploads.back();
                if (last.x0 == partial.x0 && last.x1 == partial.x1 && last.y1 + 1 >= partial.y0 && partial.y1 + 1 >= last.y0) {
                    last.y0 = std::min(last.y0, partial.y0);
                    last.y1 = std::max(last.y1, partial.y1);
                    partial = Rect{};
                }
            }
            if (!partial.empty()) s.pending_uploads.push_back(partial);
        }
        if (s.pending_uploads.size() > 64u) apply_pending_uploads(s);
    }
}

void GeGl::present(std::uint32_t fb_address, std::uint32_t stride, std::uint32_t format,
                   GLuint out_fbo, int out_width, int out_height) {
    flush();
    reset_cache(); // the code below drives GL directly
    constexpr std::uint32_t width = 480u, height = 272u;
    GLuint texture = 0u;
    float rect_w = 1.0f, rect_h = 1.0f, origin_x = 0.0f, origin_y = 0.0f;
    const std::uint32_t offset = vram_offset(fb_address);
    int ox = 0, oy = 0;
    Surface *s = offset != kNotVram ? find_surface(offset, stride, format, ox, oy) : nullptr;
    if (s != nullptr && !s->content.empty()) {
        apply_pending_uploads(*s);
        sync_overlaps(*s);
        texture = s->color;
        rect_w = static_cast<float>(width) / static_cast<float>(s->stride);
        rect_h = static_cast<float>(height) / static_cast<float>(s->height);
        origin_x = static_cast<float>(ox) / static_cast<float>(s->stride);
        origin_y = static_cast<float>(oy) / static_cast<float>(s->height);
    } else if (fb_address != 0u) {
        // CPU-drawn framebuffer: upload it as it is in guest memory.
        const std::uint32_t bpp = bpp_of(format);
        prepare_vram_read(fb_address, stride * height * bpp);
        pixels_.assign(width * height * 4u, 0u);
        for (std::uint32_t y = 0; y < height; ++y) {
            const std::uint8_t *row = memory_.raw_pointer(fb_address + y * stride * bpp, width * bpp);
            if (row == nullptr) continue;
            for (std::uint32_t x = 0; x < width; ++x) {
                std::uint32_t rgba;
                if (bpp == 4u) std::memcpy(&rgba, row + x * 4u, 4u);
                else rgba = to_rgba(static_cast<std::uint32_t>(row[x * 2u]) | (static_cast<std::uint32_t>(row[x * 2u + 1u]) << 8u), format);
                std::memcpy(&pixels_[(y * width + x) * 4u], &rgba, 4u);
            }
        }
        glBindTexture(GL_TEXTURE_2D, scratch_texture_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels_.data());
        texture = scratch_texture_;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, out_fbo);
    glViewport(0, 0, out_width, out_height);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (texture == 0u) return;
    glUseProgram(present_program_);
    glBindVertexArray(present_vao_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glUniform1i(u_present_tex_, 0);
    glUniform2f(u_present_rect_, rect_w, rect_h);
    glUniform2f(u_present_origin_, origin_x, origin_y);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    reset_cache();
}

void GeGl::report_targets() const {
    for (const auto &s : surfaces_) {
        if (!s.alive) continue;
        std::cerr << "[gl-surface] vram=0x" << std::hex << s.offset << std::dec << " " << s.stride << "x" << s.height
                  << " fmt=" << s.format << " content=" << s.content.x0 << "," << s.content.y0 << ".." << s.content.x1
                  << "," << s.content.y1 << "\n";
    }
}

} // namespace pspweb
