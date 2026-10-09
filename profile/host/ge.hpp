#pragma once

// Software implementation of the PSP graphics engine (GE).  Display lists are
// executed synchronously when enqueued; primitives are transformed, lit and
// rasterized straight into guest VRAM, so the display path only has to read
// the framebuffer the game points sceDisplay at.

#include "ge_gl.hpp"
#include "psprecomp/guest_memory.hpp"

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace pspweb {

class GeGl;

class Ge {
public:
    explicit Ge(psprecomp::GuestMemory &memory);

    // Executes a display list from `start` until END or the stall address.
    // Returns true when the list reached its end.
    bool run_list(std::uint32_t start, std::uint32_t stall);

    struct Stats {
        std::uint64_t lists{};
        std::uint64_t prims{};
        std::uint64_t triangles{};
        std::uint64_t culled{};
        std::uint64_t sprites{};
        std::uint64_t pixels{};
        std::uint64_t transfers{};
        std::uint64_t nanoseconds{};
    };
    [[nodiscard]] const Stats &stats() const noexcept { return stats_; }
    void reset_stats() { stats_ = {}; }
    // PSPWEB_GE_PROFILE: prints pixel counts per render target since the last call.
    static void report_targets();
    // Per-section GE times (decode, emit, texture, transfer, state, transfer GL
    // sync) accumulated while profiling; returns them and starts over.
    static void take_sections(double out[6]);
    // Called once per vblank; cached RAM textures are revalidated once per frame.
    void next_frame() {
        ++frame_;
        prim_in_frame_ = 0u;
        state_dirty_ = true; // cached textures revalidate once per frame
    }
    // sceGeSaveContext / sceGeRestoreContext: the state as a list of GE commands
    // (games treat the context buffer as opaque, and it holds up to 512 words).
    [[nodiscard]] std::vector<std::uint32_t> save_context() const;
    void restore_context(const std::vector<std::uint32_t> &commands);
    // Routes primitives to the GL backend instead of the software rasterizer.
    void set_gl(GeGl *gl);
    // Marks guest VRAM bytes as rewritten so cached textures there revalidate.
    void touch_vram(std::uint32_t address, std::uint32_t bytes);

private:
    // Drawing-space vertex: pixels, depth 0..65535, texels, colour 0..255.
    struct Vertex {
        float x{}, y{}, z{};
        float w{1.0f};
        float u{}, v{};
        float r{}, g{}, b{}, a{};
    };
    // Clip-space vertex (drawing space directly in through mode).
    struct ClipVertex {
        float cx{}, cy{}, cz{}, cw{1.0f};
        float u{}, v{};
        float r{}, g{}, b{}, a{};
    };
    struct Layout {
        std::uint32_t tc{}, col{}, nrm{}, pos{}, weight{}, idx{};
        std::uint32_t weights{1}, morphs{1};
        std::uint32_t weight_off{}, tc_off{}, col_off{}, nrm_off{}, pos_off{}, size{};
        bool through{};
    };

    void execute(std::uint32_t op);
    [[nodiscard]] std::uint32_t relative_address(std::uint32_t data) const;
    void draw_primitive(std::uint32_t type, std::uint32_t count);
    void block_transfer();
    void load_clut();

    void compute_layout();
    void decode_vertex(std::uint32_t address, ClipVertex &out) const;
    [[nodiscard]] Vertex to_screen(const ClipVertex &cv) const;
    void light_vertex(const float pos[3], const float normal[3], float color[4]) const;

    void draw_triangle(const ClipVertex &a, const ClipVertex &b, const ClipVertex &c);
    void raster_triangle(const Vertex &a, const Vertex &b, const Vertex &c);
    void draw_sprite(const Vertex &a, const Vertex &b);
    void emit_triangle(const Vertex &a, const Vertex &b, const Vertex &c);
    void emit_sprite(const Vertex &a, const Vertex &b);
    void submit_gl_state();
    void shade_pixel(int x, int y, float z, float u, float v, float r, float g, float b, float a);

    void sample_texture(float u, float v, float rgba[4]) const;
    [[nodiscard]] std::uint32_t fetch_texel(int x, int y) const;
    [[nodiscard]] std::uint32_t clut_lookup(std::uint32_t index) const;
    [[nodiscard]] std::uint32_t read_pixel(std::uint32_t address) const;
    void write_pixel(std::uint32_t address, std::uint32_t rgba, std::uint32_t old);

    [[nodiscard]] static float cmd_float(std::uint32_t data);

    // Per-primitive state for the pixel pipeline, derived from the registers.
    struct PixelState {
        std::uint8_t *vram{};
        std::uint32_t fb_offset{}, fb_stride{}, fb_format{}, fb_bpp{};
        std::uint32_t z_offset{}, z_stride{}, z_func{};
        bool z_test{}, z_write{};
        bool clear{};
        std::uint32_t clear_flags{};
        bool texture{}, tex_alpha{}, tex_double{};
        std::uint32_t tex_func{};
        float env[3]{};
        bool alpha_test{};
        std::uint32_t alpha_func{}, alpha_ref{}, alpha_mask{};
        bool blend{};
        std::uint32_t src_factor{}, dst_factor{}, equation{};
        float fix_a[3]{}, fix_b[3]{};
        std::uint32_t write_mask{};
        int sx1{}, sy1{}, sx2{}, sy2{};
    };
    void prepare_pixel_state();

    // Decoded level-0 textures.
    struct TextureKey {
        std::uint32_t address, format, width, height, buf_width, swizzle, clut_mode;
        std::uint64_t clut_hash;
        bool operator==(const TextureKey &) const = default;
    };
    struct TextureKeyHash {
        std::size_t operator()(const TextureKey &k) const noexcept {
            std::uint64_t h = k.address * 0x9E3779B97F4A7C15ull ^ k.format ^ (static_cast<std::uint64_t>(k.width) << 8u) ^
                              (static_cast<std::uint64_t>(k.height) << 20u) ^ (static_cast<std::uint64_t>(k.buf_width) << 32u) ^
                              (static_cast<std::uint64_t>(k.swizzle) << 44u) ^ (static_cast<std::uint64_t>(k.clut_mode) << 45u) ^
                              k.clut_hash;
            return static_cast<std::size_t>(h ^ (h >> 29u));
        }
    };
    struct CachedTexture {
        std::vector<std::uint32_t> texels;
        std::uint64_t source_hash{};
        std::uint32_t frame{};
        std::uint64_t vram_generation{};
    };
    void bind_texture();
    void evict_textures(std::uint64_t need);
    [[nodiscard]] std::uint32_t texture_bytes(std::uint32_t format, std::uint32_t buf_width, std::uint32_t height) const;

    PixelState ps_{};
    GeGl *gl_{};
    // Reused per-primitive buffers (avoids two heap allocations per primitive).
    std::vector<ClipVertex> scratch_vertices_;
    std::vector<std::uint32_t> scratch_indices_;
    // Host view of the current primitive's vertex data (null: use GuestMemory).
    const std::uint8_t *vertex_raw_{};
    std::uint32_t vertex_raw_base_{};
    std::unordered_map<TextureKey, CachedTexture, TextureKeyHash> textures_;
    const CachedTexture *texture_{};
    int texture_width_{1}, texture_height_{1};
    std::uint64_t texture_cache_texels_{};
    std::uint64_t clut_hash_{};
    std::uint32_t frame_{1};
    std::uint32_t prim_in_frame_{}; // for PSPWEB_DUMP_FRAME / PSPWEB_SKIP_PRIMS
    void log_primitive(std::uint32_t index, std::uint32_t type, std::uint32_t count);
    void dump_texture(const CachedTexture &texture, std::uint32_t address, std::uint32_t format, std::uint32_t width,
                      std::uint32_t height);
    std::uint64_t vram_generation_{1};
    // Per-4 KiB VRAM page write stamps; a VRAM texture is trusted while the
    // newest stamp of its pages is unchanged.
    std::array<std::uint64_t, 512> vram_pages_{};
    std::uint64_t vram_write_counter_{1};
    [[nodiscard]] std::uint64_t vram_stamp(std::uint32_t address, std::uint32_t bytes) const;
    // World * view * projection for unlit, unskinned primitives.
    std::array<float, 16> wvp_{};
    bool use_wvp_{};
    // Render state and the combined matrix are rebuilt only after the
    // registers they depend on change (most consecutive primitives share them).
    bool state_dirty_{true};
    bool matrix_dirty_{true};
    // proj * view, rebuilt only when either changes (the world matrix changes far more often).
    std::array<float, 16> pv_{};
    bool pv_dirty_{true};
    bool submitted_through_{};
    float viewport_[6]{}; // x/y/z scale, x/y/z centre (minus the drawing offset for x/y)
    std::vector<GlVertex> scratch_screen_;
    std::vector<std::uint8_t> scratch_inside_;
    std::vector<std::uint32_t> scratch_batch_index_;
    // Lighting and texture-mapping parameters decoded from the registers once
    // per change instead of once per vertex.
    struct LightSetup {
        float mat_ambient[3]{}, mat_diffuse[3]{}, emissive[3]{}, global[3]{};
        float alpha{};
        float global_alpha{1}; // the global ambient's alpha scales the lit alpha
        std::uint32_t update{};
        int count{};
        struct Light {
            std::uint32_t type{};
            float pos[3]{}, att[3]{}, ambient[3]{}, diffuse[3]{};
        } lights[4];
    } light_{};
    struct TexSetup {
        float su{1}, sv{1}, ou{}, ov{}, tw{1}, th{1};
    } tex_{};
    bool light_dirty_{true}, tex_dirty_{true};
    void build_light_setup();
    void build_tex_setup();

    psprecomp::GuestMemory &memory_;
    std::array<std::uint32_t, 256> regs_{};
    std::uint32_t pc_{};
    std::uint32_t offset_{};
    std::uint32_t vertex_address_{};
    std::uint32_t index_address_{};
    std::vector<std::uint32_t> call_stack_;
    Layout layout_{};

    std::array<float, 12> world_{};
    std::array<float, 12> view_{};
    std::array<float, 16> proj_{};
    std::array<float, 12> tgen_{};
    std::array<std::array<float, 12>, 8> bones_{};
    std::uint32_t world_index_{}, view_index_{}, proj_index_{}, tgen_index_{}, bone_index_{};
    std::array<float, 8> morph_weights_{};
    std::array<std::uint8_t, 2048> clut_{};

    Stats stats_{};
};

} // namespace pspweb
