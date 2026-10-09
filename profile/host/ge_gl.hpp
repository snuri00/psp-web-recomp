#pragma once

// OpenGL ES 3 / WebGL2 backend for the GE.  The Ge front end keeps vertex
// decoding, transform, lighting and clipping on the CPU and hands this class
// drawing-space triangles plus the render state of each primitive.  Triangles
// sharing a state are batched into one draw call.
//
// Framebuffers live in GL "surfaces": one framebuffer object per VRAM region,
// stride and pixel format.  A framebuffer address that falls inside an existing
// surface of the same layout (games pack bloom chains side by side, or point
// the GE one row into a buffer) is drawn into that surface at an offset, and a
// texture read from such an address samples the surface directly.  Guest VRAM
// is only synchronized when a block transfer, a CPU-side texture decode, a
// reinterpretation in another pixel format, or the display needs it.

#include "psprecomp/guest_memory.hpp"

#include <GLES3/gl3.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <unordered_map>
#include <vector>

namespace pspweb {

struct GlVertex {
    float x, y, z, w;   // drawing-space pixels, depth 0..65535, clip w
    float u, v;         // texels
    std::uint8_t r, g, b, a;
};

// A rectangle copied by a GE block transfer.
struct TransferRect {
    std::uint32_t base{}, stride{}, x{}, y{}, width{}, height{}, bpp{};
};

struct GlDrawState {
    std::uint32_t fb_offset{}, fb_stride{}, fb_format{};
    int sx1{}, sy1{}, sx2{}, sy2{};
    bool clear{};
    std::uint32_t clear_flags{};
    bool z_test{}, z_write{};
    std::uint32_t z_func{};
    bool blend{};
    std::uint32_t src_factor{}, dst_factor{}, equation{}, fix_a{}, fix_b{};
    bool alpha_test{};
    std::uint32_t alpha_func{}, alpha_ref{}, alpha_mask{};
    bool stencil{};
    std::uint32_t stencil_func{}, stencil_ops{}; // raw STST / SOP registers
    std::uint32_t mask_rgb{}, mask_alpha{};
    // Fog: factor = w * fog_w + fog_bias per vertex (w is the clip w, a fixed
    // multiple of the eye-space depth under a perspective projection).
    bool fog{};
    float fog_w{}, fog_bias{};
    std::uint32_t fog_color{};
    bool texture{};
    std::uint32_t tex_func{};
    bool tex_alpha{}, tex_double{};
    std::uint32_t env{};
    bool clamp_u{}, clamp_v{}, linear{};
    // Texture source: decoded texels, or a surface region at tex_address.
    const std::uint32_t *texels{};
    const void *texture_identity{};
    std::uint64_t texture_hash{};
    std::uint32_t tex_width{}, tex_height{};
    bool tex_from_target{};
    std::uint32_t tex_address{}, tex_format{}, tex_buf_width{};

    bool operator==(const GlDrawState &) const = default;
};

class GeGl {
public:
    explicit GeGl(psprecomp::GuestMemory &memory);
    ~GeGl();

    // Compiles shaders; a GL ES 3 context must be current.
    bool init();

    // True when a texture at `address` (format 0..3, buffer width in pixels)
    // lies inside GPU-drawn surface content and can be sampled from it.
    [[nodiscard]] bool is_gpu_target(std::uint32_t address, std::uint32_t format, std::uint32_t buf_width) const;
    // Makes VRAM current for a range the CPU is about to read (texture decode).
    // Returns true when GPU pixels were downloaded into the range.
    bool prepare_vram_read(std::uint32_t address, std::uint32_t bytes);

    void set_state(const GlDrawState &state);
    void triangle(const GlVertex &a, const GlVertex &b, const GlVertex &c);
    // Indexed form: add vertices once, then triangles by batch-relative index.
    std::uint32_t vertex(const GlVertex &v) {
        batch_.push_back(v);
        return static_cast<std::uint32_t>(batch_.size() - 1u);
    }
    void indexed_triangle(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        indices_.push_back(a);
        indices_.push_back(b);
        indices_.push_back(c);
    }
    void flush();
    void forget_texture(const void *identity);

    // Keep guest VRAM and surfaces coherent around a block transfer.
    void before_transfer(const TransferRect &src, const TransferRect &dst);
    void after_transfer(const TransferRect &dst);

    // Draws the displayed framebuffer into `out_fbo` (0 = the canvas).
    void present(std::uint32_t fb_address, std::uint32_t stride, std::uint32_t format,
                 GLuint out_fbo, int out_width, int out_height);
    void next_frame() { ++frame_; }
    // Internal resolution as a multiple of the PSP's (1 = native 480x272).
    // Changing it writes GPU-only pixels back to VRAM and rebuilds every
    // surface at the new size.  Returns the scale actually used, which the GL
    // implementation's texture size limit may lower.
    int set_render_scale(int scale);
    [[nodiscard]] int render_scale() const noexcept { return scale_; }
    // Highest scale the GL implementation's size limits allow.
    [[nodiscard]] int max_render_scale() const;
    // Called whenever GPU pixels are written back into guest VRAM.
    void set_vram_write_hook(std::function<void(std::uint32_t, std::uint32_t)> hook) { vram_write_hook_ = std::move(hook); }

    [[nodiscard]] std::uint64_t draw_calls() const noexcept { return draw_calls_; }
    [[nodiscard]] std::uint64_t downloads() const noexcept { return downloads_; }
    [[nodiscard]] std::uint64_t uploads() const noexcept { return uploads_; }
    // Milliseconds spent issuing draw batches (only measured with PSPWEB_FRAME_PROFILE).
    [[nodiscard]] double flush_ms() const noexcept { return flush_ms_; }
    [[nodiscard]] std::uint64_t upload_pixels() const noexcept { return upload_pixels_; }
    void reset_counters() { draw_calls_ = 0; downloads_ = 0; uploads_ = 0; upload_pixels_ = 0; flush_ms_ = 0.0; }
    void report_targets() const;

private:
    // Inclusive pixel rectangle; empty when x1 < x0.
    struct Rect {
        int x0{0}, y0{0}, x1{-1}, y1{-1};
        [[nodiscard]] bool empty() const { return x1 < x0 || y1 < y0; }
        void add(int ax0, int ay0, int ax1, int ay1);
        void add(const Rect &r) { add(r.x0, r.y0, r.x1, r.y1); }
        [[nodiscard]] bool intersects(const Rect &o) const {
            return !empty() && !o.empty() && x0 <= o.x1 && o.x0 <= x1 && y0 <= o.y1 && o.y0 <= y1;
        }
        [[nodiscard]] Rect clip(const Rect &o) const {
            if (!intersects(o)) return Rect{};
            return Rect{std::max(x0, o.x0), std::max(y0, o.y0), std::min(x1, o.x1), std::min(y1, o.y1)};
        }
        [[nodiscard]] Rect shifted(int dx, int dy) const {
            return empty() ? Rect{} : Rect{x0 + dx, y0 + dy, x1 + dx, y1 + dy};
        }
    };
    struct Surface {
        std::uint32_t offset{}, stride{}, format{}, height{};
        GLuint fbo{}, color{}, depth{};
        Rect gpu_dirty;  // drawn by the GPU since the last download to VRAM
        Rect content;    // everything the GPU has drawn
        bool alive{true};
        std::uint64_t last_draw{};  // draw sequence of the newest batch drawn into it
        std::vector<Rect> pending_uploads; // VRAM rectangles a block transfer rewrote
        // The PSP keeps the stencil in the framebuffer's alpha.  Here it lives
        // in the GL stencil buffer while drawing and is copied into the colour
        // alpha (or back) only when the other side is about to be used.
        // Never overlapping: inside alpha_stale the stencil is newer than the
        // colour alpha, inside stencil_stale the colour alpha is newer.
        Rect alpha_stale;
        Rect stencil_stale;
        // Bits that may be set anywhere in the stencil / colour alpha; resolves
        // skip the others (games tend to use one or two stencil bits).
        std::uint32_t stencil_bits{}, alpha_bits{0xFFu};
        int stale_value{-1};  // the stencil value throughout alpha_stale, when one rectangle set it
        // Exact per-row extents behind the bounding rectangles above, so a block
        // transfer that overwrites whole rows of stale GPU output (games reuse
        // post-processing buffers for texture uploads) just retires those rows.
        struct RowSpan {
            int cx0{0}, cx1{-1};  // content
            int dx0{0}, dx1{-1};  // GPU dirty
        };
        std::vector<RowSpan> rows;
        // Newest draw of each overlapping surface already copied into this one.
        std::vector<std::pair<const Surface *, std::uint64_t>> synced;
    };
    struct GlTexture {
        GLuint id{};
        std::uint64_t hash{};
        std::uint32_t width{}, height{};
    };

    [[nodiscard]] static std::uint32_t bpp_of(std::uint32_t format) { return format == 3u ? 4u : 2u; }
    [[nodiscard]] std::uint32_t row_bytes(const Surface &s) const { return s.stride * bpp_of(s.format); }
    [[nodiscard]] std::uint32_t surface_end(const Surface &s) const { return s.offset + row_bytes(s) * s.height; }
    // Position of VRAM `offset` inside `s` in pixels; false when outside.
    [[nodiscard]] bool locate(const Surface &s, std::uint32_t offset, int &ox, int &oy) const;
    // Surface and origin for drawing at `offset`; creates, grows or merges surfaces.
    Surface &surface_for_draw(std::uint32_t offset, std::uint32_t stride, std::uint32_t format,
                              std::uint32_t rows, int &ox, int &oy);
    Surface *find_surface(std::uint32_t offset, std::uint32_t stride, std::uint32_t format, int &ox, int &oy);
    void allocate(Surface &s, std::uint32_t height);
    void grow(Surface &s, std::uint32_t height);
    void absorb(Surface &into, Surface &from);
    // Brings `s` up to date with GPU pixels of overlapping surfaces in another layout.
    void sync_overlaps(Surface &s);
    // Copies `from` into `into` on the GPU, reinterpreting 32-bit pixels as
    // pairs of 16-bit ones (or back); false when the layouts are incompatible.
    bool reinterpret(Surface &into, const Surface &from, const Rect &hit);
    void download(Surface &s);
    void mark_drawn(Surface &s, const Rect &r);
    void recompute_bounds(Surface &s);
    // Calls f(row, x0, x1) for each piece of `rect` that lands in surface `s`.
    template <typename F>
    void for_each_segment(const Surface &s, const TransferRect &rect, F &&f) const;
    // Uploads rectangles block transfers wrote, before the surface is used.
    void apply_pending_uploads(Surface &s);
    void upload_rect(Surface &s, const Rect &rect);
    // Part of a VRAM rectangle that falls inside `s`, in surface pixels.
    [[nodiscard]] Rect overlap(const Surface &s, const TransferRect &rect) const;
    [[nodiscard]] bool batch_uses(const Surface &s) const;
    GLuint texture_for_state(Surface *source, int ox, int oy);
    [[nodiscard]] static Rect whole(const Surface &s) {
        return Rect{0, 0, static_cast<int>(s.stride) - 1, static_cast<int>(s.height) - 1};
    }
    // stencil -> colour alpha inside `region`.  With fewer than all `bits`, only
    // those bits are made correct and the area stays marked stale.
    void resolve_alpha(Surface &s, const Rect &region, std::uint32_t bits = 0xFFu);
    // Bounds of the pending batch in the PSP pixels of the drawing-space
    // (unshifted) frame, and whether it is a single axis-aligned rectangle.
    [[nodiscard]] Rect batch_bounds(bool &rectangle) const;
    void resolve_stencil(Surface &s);                    // colour alpha -> stencil where it is stale
    // Call before colour alpha in `r` is overwritten from VRAM or another surface.
    void alpha_will_change(Surface &s, const Rect &r);
    bool batch_writes_stencil_{};
    int batch_stencil_value_{-1}; // value a rectangular stencil batch sets everywhere it covers
    GLsizei scratch_width_{}, scratch_height_{};
    GLint u_tex_opaque_{};
    void apply_state();
    // glBlitFramebuffer with the scissor test off and all colour channels on.
    void blit(GLuint from, const GLint src[4], GLuint to, const GLint dst[4], GLbitfield mask);

    // Cached GL state: skips redundant calls, which are costly through WebGL.
    struct Cache {
        GLuint program{~0u}, vao{~0u}, fbo{~0u}, texture{~0u}, sampler{~0u};
        GLint viewport_w{-1}, viewport_h{-1};
        GLint scissor[4]{-1, -1, -1, -1};
        int scissor_test{-1}, depth_test{-1}, blend{-1}, stencil_test{-1};
        std::uint64_t stencil_key{~0ull};
        GLenum depth_func{0}, blend_src{0}, blend_dst{0}, blend_eq{0};
        int depth_mask{-1};
        int color_mask{-1};
        float blend_color[4]{-1, -1, -1, -1};
        float uniforms[32][4];
        bool uniform_valid[32]{};
    };
    void reset_cache() { cache_ = Cache{}; }
    void use_program(GLuint program);
    void bind_framebuffer(GLuint fbo);
    void bind_texture0(GLuint texture);
    void set_enabled(GLenum cap, int &cached, bool on);
    void set_uniform(GLint location, float a, float b = 0.0f, float c = 0.0f, int count = 1);
    void set_uniform_int(GLint location, int value);
    Cache cache_{};
    GLuint samplers_[8]{};

    psprecomp::GuestMemory &memory_;
    GLuint program_{}, present_program_{};
    GLuint vao_{}, vbo_{}, present_vao_{}, scratch_texture_{}, blank_texture_{};
    // Each draw gets its own small index buffer from a rotating pool: some
    // browsers re-validate a whole index buffer after any change to it.
    std::vector<GLuint> index_buffers_;
    std::size_t next_index_buffer_{};
    std::vector<std::uint32_t> indices_;
    GLint u_target_{}, u_origin_{}, u_tex_{}, u_tex_enable_{}, u_tex_size_{}, u_tex_origin_{}, u_tex_func_{};
    GLint u_tex_alpha_{}, u_tex_double_{}, u_env_{}, u_alpha_test_{}, u_alpha_func_{}, u_alpha_ref_{}, u_alpha_mask_{};
    GLint u_src_scale_{}, u_clear_{}, u_tex_window_{}, u_tex_clamp_{};
    GLint u_fog_{}, u_fog_enable_{}, u_fog_color_{};
    GLint u_present_tex_{}, u_present_rect_{}, u_present_origin_{};
    GLuint reinterpret_program_{};
    GLint u_ri_src_{}, u_ri_mode_{}, u_ri_format_{}, u_ri_delta_{}, u_ri_row_bytes_{}, u_ri_src_size_{};
    std::uint64_t draw_sequence_{};
    // Surfaces are scale_ times larger than the PSP pixels they hold; uploads
    // and downloads pass through a native-size staging framebuffer.
    int scale_{1};
    GLuint staging_fbo_{}, staging_texture_{};
    GLint u_ri_scale_{};
    GLuint stencil_program_{}, alpha_program_{}, stencil_fbo_{};
    GLint u_st_src_{}, u_st_bit_{}, u_al_value_{};
    GLint clear_alpha_{};

    std::deque<Surface> surfaces_;
    std::unordered_map<const void *, GlTexture> textures_;
    std::vector<GlVertex> batch_;
    GlDrawState state_{};
    bool state_valid_{};
    Surface *current_{};
    int current_ox_{}, current_oy_{};
    std::uint32_t frame_{1};
    std::uint64_t draw_calls_{};
public:
    // Extra GL passes per kind, for profiling.
    struct PassCounters {
        std::uint64_t alpha_resolves{}, stencil_resolves{}, reinterprets{}, feedback_copies{}, blits{};
        std::uint64_t pixels{}; // output pixels those passes touched, at the internal resolution
    };
    [[nodiscard]] const PassCounters &passes() const noexcept { return passes_; }
    void reset_passes() { passes_ = PassCounters{}; }
private:
    PassCounters passes_{};
    std::uint64_t downloads_{};
    std::uint64_t uploads_{}, upload_pixels_{};
    double flush_ms_{};
    std::vector<std::uint8_t> pixels_;
    std::function<void(std::uint32_t, std::uint32_t)> vram_write_hook_;
};

} // namespace pspweb
