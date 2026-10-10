// license:BSD-3-Clause
// copyright-holders:Olivier Galibert, R. Belmont, ElSemi, Angelo Salese
//
// Model 2 screen output, transplanted from MAME (see video.h): segaic24's
// tile_info, draw_rect (rgb32) and draw_common; model2_state::palette_w,
// colorxlat_w, horizontal/vertical_sync_w, render_polygons' frame logic and
// screen_update. MAME's tilemap engine is replaced by build_layer (the same
// pixmap and flags MAME's tilemap caches hold) and tilemap_draw (the same
// pixel rule as tilemap_t::draw with one scroll value).

#include "runtime/video.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>

#if defined(__SWITCH__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include <switch.h>
#pragma GCC diagnostic pop
#endif

namespace rt {

namespace {
constexpr uint8_t PIXEL_LAYER0 = 0x10;  // TILEMAP_PIXEL_LAYER0
constexpr uint8_t CATEGORY_MASK = 0x0f; // TILEMAP_PIXEL_CATEGORY_MASK
constexpr int DRAW_OPAQUE = 0x80;       // TILEMAP_DRAW_OPAQUE
inline uint16_t le16(const uint8_t *b, uint32_t i) { return uint16_t(b[i * 2] | b[i * 2 + 1] << 8); }
inline uint32_t rgb(uint32_t r, uint32_t g, uint32_t b) { return 0xff000000u | (r << 16) | (g << 8) | b; }
} // namespace

Video::Video(const uint8_t *tile_ram, const uint8_t *char_ram)
    : tile_src_(tile_ram), pen_tab_(pens_), tile_ram_(tile_ram), char_ram_(char_ram), screen_(size_t(W) * H), sys24_(size_t(W) * (H + 4)),
      background_gpu_(size_t(W) * H), foreground_gpu_(size_t(W) * H), gpu_tile_words_(kGpuTileWords),
      gpu_pens_(kGpuPens) {
    static uint64_t instances = 0;
    instance_ = ++instances;
    for (auto &p : pens_) p = rgb(0, 0, 0); // palette_device starts black
    for (int i = 0; i < 256; i++) gamma_[i] = uint8_t(std::max((double(i) - 64.0) * 255.0 / 191.0, 0.0));
    for (int l = 0; l < 4; l++) pixmap_[l].assign(512 * 512, 0), flags_[l].assign(512 * 512, 0);
    system24_tile_generations_.resize(4 * 4096);
#ifndef M2_VITA_RENDER_OPT
    dec_chars_.resize(0x80000);
    dec_char_dirty_.resize(0x4000);
    dec_tiles_.resize(4 * 4096);
#endif
#ifdef M2_VITA_RENDER_OPT
    character_copy_.resize(0x80000);
    character_dirty_.resize(0x4000);
    tile_ram_copy_.resize(0x10000);
    tile_values_.resize(4 * 4096);
    background_.resize(size_t(W) * (H + 4));
#endif
}

Video::~Video() { set_threaded(false); }

void Video::set_threaded(bool on) {
    if (on == threaded_) return;
    if (!on) {
        sync();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            quit_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        quit_ = false;
        screen_ = display_; // keep showing the last finished picture
        threaded_ = false;
        tile_src_ = tile_ram_;
        pen_tab_ = pens_;
        return;
    }
    tile_snap_.resize(0x10000);
    palram_snap_.resize(0x4000);
    xlat_snap_.resize(0xc000);
    luma_snap_.resize(0x20000);
    pens_snap_.resize(8192);
    display_ = screen_;
    threaded_ = true;
    worker_ = std::thread(&Video::worker_loop, this);
}

void Video::sync() const {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return !job_pending_ && !job_busy_; });
}

void Video::worker_loop() {
#if defined(__SWITCH__)
    // Prefers core 1; may use core 0 when the emulation thread idles (it has
    // the higher priority there).
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, 1, 0b0111);
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2C);
#endif
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        cv_.wait(lock, [this] { return job_pending_ || quit_; });
        if (job_pending_) {
            job_pending_ = false;
            job_busy_ = true;
            const Job job = job_;
            lock.unlock();
            VideoProfile profile{};
            compose(job, profile);
            lock.lock();
            job_profile_ = profile;
            job_busy_ = false;
            cv_.notify_all();
            continue;
        }
        if (quit_) return;
    }
}

void Video::palette_w(uint32_t offset, const uint8_t *palram, const uint8_t *colorxlat) {
    const uint16_t palcolor = le16(palram, offset);
    const uint8_t r = uint8_t(le16(colorxlat, (0x0080 >> 1) + (((palcolor >> 0) & 0x1f) << 8)));
    const uint8_t g = uint8_t(le16(colorxlat, (0x4080 >> 1) + (((palcolor >> 5) & 0x1f) << 8)));
    const uint8_t b = uint8_t(le16(colorxlat, (0x8080 >> 1) + (((palcolor >> 10) & 0x1f) << 8)));
    const uint32_t pen = rgb(gamma_[r], gamma_[g], gamma_[b]);
    if (pens_[offset & 0x1fff] != pen) {
#ifdef M2_VITA_RENDER_OPT
        background_dirty_ = foreground_dirty_ = true;
#endif
        system24_source_dirty_ = true;
        system24_palette_generation_ = system24_texture_generation_ + 1;
    }
    pens_[offset & 0x1fff] = pen;
}

// segaic24 tile_info + MAME tilemap pixmap: 64x64 tiles (TILEMAP_SCAN_ROWS)
// of 8x8, 4bpp chars (char_layout, bit order swapped within 16-bit words),
// pen = color * 16 + pixel, pen 0 transparent, category = tile bit 15.
void Video::build_layer(int layer) {
    const uint32_t base = uint32_t(layer) * 0x1000; // tile_info_0s/0w/1s/1w
    uint16_t *pm = pixmap_[layer].data();
    uint8_t *fm = flags_[layer].data();
    for (uint32_t t = 0; t < 64 * 64; t++) {
        const uint16_t val = tile(t | base);
        const uint32_t code = val & 0x3fff;
#ifdef M2_VITA_RENDER_OPT
        uint16_t &previous = tile_values_[base + t];
        if (tiles_valid_ && previous == val && !character_dirty_[code]) continue;
        if (!tiles_valid_) background_dirty_ = foreground_dirty_ = true;
        else {
            (previous & 0x8000 ? foreground_dirty_ : background_dirty_) = true;
            (val & 0x8000 ? foreground_dirty_ : background_dirty_) = true;
        }
        previous = val;
#else
        // Desktop: only tiles whose value or character changed (decode_layers).
        uint16_t &previous = dec_tiles_[base + t];
        if (dec_valid_ && previous == val && !dec_char_dirty_[code]) continue;
        previous = val;
#endif
        ++profile_.tiles_rebuilt;
        system24_source_dirty_ = true;
        system24_tile_generations_[base + t] = system24_texture_generation_ + 1;
        const uint32_t color = (val >> 7) & 0xff;
        const uint8_t category = (val & 0x8000) ? 1 : 0;
        const uint32_t tx = (t & 63) * 8, ty = (t >> 6) * 8;
        for (uint32_t y = 0; y < 8; y++)
            for (uint32_t x = 0; x < 8; x++) {
                const uint32_t b = code * 32 + y * 4 + (x >> 1);
                const uint8_t byte = char_ram_[b ^ 1];
                const uint8_t pix = (x & 1) ? (byte & 0x0f) : (byte >> 4);
                const size_t i = size_t(ty + y) * 512 + (tx + x);
                pm[i] = uint16_t(color * 16 + pix);
                fm[i] = uint8_t(category | (pix ? PIXEL_LAYER0 : 0));
            }
    }
}

#ifndef M2_VITA_RENDER_OPT
// The four layers' pixmaps, re-decoding only tiles whose tile value or
// character changed since the last frame: the same pixmaps as decoding all
// 16,384 tiles every frame, at a fraction of the cost (in a race only the
// HUD's digits change). Characters are compared (256-byte pages, then 32-byte
// characters) only on frames the game wrote character RAM.
void Video::decode_layers() {
    if (dec_valid_ && (character_memory_touched_ || !write_tracking_)) {
        std::fill(dec_char_dirty_.begin(), dec_char_dirty_.end(), uint8_t(0));
        constexpr size_t kPage = 256;
        for (size_t page = 0; page < dec_chars_.size(); page += kPage) {
            if (std::memcmp(char_ram_ + page, dec_chars_.data() + page, kPage) == 0) continue;
            for (size_t c = page / 32; c < (page + kPage) / 32; ++c)
                if (std::memcmp(char_ram_ + c * 32, dec_chars_.data() + c * 32, 32) != 0) {
                    dec_char_dirty_[c] = 1;
                    ++profile_.characters_changed;
                }
            std::memcpy(dec_chars_.data() + page, char_ram_ + page, kPage);
        }
    } else if (dec_valid_) {
        std::fill(dec_char_dirty_.begin(), dec_char_dirty_.end(), uint8_t(0));
    } else {
        std::memcpy(dec_chars_.data(), char_ram_, dec_chars_.size());
    }
    for (int l = 0; l < 4; l++) build_layer(l);
    dec_valid_ = true;
    character_memory_touched_ = tile_memory_touched_ = false;
}
#endif

#ifdef M2_VITA_RENDER_OPT
void Video::update_tile_cache() {
    const bool chars_changed = !tiles_valid_ || (write_tracking_ ? character_memory_touched_ :
        std::memcmp(char_ram_, character_copy_.data(), character_copy_.size()) != 0);
    const bool ram_changed = !tiles_valid_ || (write_tracking_ ? tile_memory_touched_ :
        std::memcmp(tile_ram_, tile_ram_copy_.data(), tile_ram_copy_.size()) != 0);
    if (!chars_changed && !ram_changed) return;
    std::fill(character_dirty_.begin(), character_dirty_.end(), uint8_t(0));
    if (chars_changed) {
        constexpr size_t page_bytes = 256;
        constexpr size_t chars_per_page = page_bytes / 32;
        for (size_t page = 0; page < character_copy_.size(); page += page_bytes) {
            if (tiles_valid_ && std::memcmp(char_ram_ + page, character_copy_.data() + page, page_bytes) == 0)
                continue;
            const size_t first = page / 32;
            for (size_t local = 0; local < chars_per_page; ++local) {
                const size_t code = first + local;
                const size_t offset = code * 32;
                if (!tiles_valid_ || std::memcmp(char_ram_ + offset, character_copy_.data() + offset, 32) != 0) {
                    std::memcpy(character_copy_.data() + offset, char_ram_ + offset, 32);
                    character_dirty_[code] = 1;
                    ++profile_.characters_changed;
                }
            }
        }
    }
    bool draw_state_changed = !tiles_valid_;
    if (tiles_valid_ && ram_changed) {
        auto changed = [&](size_t offset, size_t bytes) {
            return std::memcmp(tile_ram_ + offset, tile_ram_copy_.data() + offset, bytes) != 0;
        };
        // Line-scroll tables, layer control/scroll registers and window masks.
        draw_state_changed = changed(0x8000, 0x1000) || changed(0xa000, 0x10) || changed(0xc000, 0x2000);
    }
    for (int layer = 0; layer < 4; ++layer) build_layer(layer);
    // Normal-mode opaque backgrounds ignore category, whereas split modes
    // still filter category 0. Changing modes changes uploaded alpha even
    // when tile and character RAM are unchanged.
    if (tiles_valid_ && ram_changed &&
        bool(tile(0x5006) & 0x6000) != bool(le16(tile_ram_copy_.data(), 0x5006) & 0x6000)) {
        std::fill(system24_tile_generations_.begin() + 2 * 4096,
                  system24_tile_generations_.end(), system24_texture_generation_ + 1);
        system24_source_dirty_ = true;
    }
    // The remaining tile RAM contains scrolling, window masks and line tables.
    // Any change there invalidates composition even when no glyph was rebuilt.
    if (ram_changed) std::memcpy(tile_ram_copy_.data(), tile_ram_, tile_ram_copy_.size());
    if (draw_state_changed) background_dirty_ = foreground_dirty_ = true;
    tiles_valid_ = true;
    tile_memory_touched_ = character_memory_touched_ = false;
}
#endif

// segaic24 draw_rect, rgb32 version (model 1/2): copy a rectangle of the
// layer's pixmap to the bitmap through the 8-pixel window mask.
void Video::draw_rect(std::vector<uint32_t> &dm, const uint16_t *mask, uint16_t tpri, int flags, int win, int L, int sx,
                      int sy, int xx1, int yy1, int xx2, int yy2) {
    const uint16_t *source = &pixmap_[L][size_t(sy) * 512 + size_t(sx)];
    const uint8_t *trans = &flags_[L][size_t(sy) * 512 + size_t(sx)];
    uint32_t *dest = &dm[size_t(yy1) * size_t(dw_) + size_t(xx1)];
    tpri |= PIXEL_LAYER0;
    mask += yy1 * 4;
    yy2 -= yy1;
    while (xx1 >= 128) {
        xx1 -= 128;
        xx2 -= 128;
        mask++;
    }
    for (int y = 0; y < yy2; y++) {
        const uint16_t *src = source;
        const uint8_t *srct = trans;
        uint32_t *dst = dest;
        const uint16_t *mask1 = mask;
        int llx = xx2;
        int cur_x = xx1;
        while (llx > 0) {
            uint16_t m = *mask1++;
            if (win) m = uint16_t(~m);
            if (!cur_x && llx >= 128) {
                if (!m) {
                    for (int x = 0; x < 128; x++) {
                        if (*srct++ == tpri || (flags & DRAW_OPAQUE)) *dst = pen_tab_[*src];
                        src++;
                        dst++;
                    }
                } else if (m == 0xffff) {
                    src += 128;
                    srct += 128;
                    dst += 128;
                } else {
                    for (int x = 0; x < 128; x += 8) {
                        if (!(m & 0x8000))
                            for (int xx = 0; xx < 8; xx++)
                                if (srct[xx] == tpri || (flags & DRAW_OPAQUE)) dst[xx] = pen_tab_[src[xx]];
                        src += 8;
                        srct += 8;
                        dst += 8;
                        m = uint16_t(m << 1);
                    }
                }
            } else {
                const int llx1 = llx >= 128 ? 128 : llx;
                if (!m) {
                    for (int x = cur_x; x < llx1; x++) {
                        if (*srct++ == tpri || (flags & DRAW_OPAQUE)) *dst = pen_tab_[*src];
                        src++;
                        dst++;
                    }
                } else if (m == 0xffff) {
                    src += 128 - cur_x;
                    srct += 128 - cur_x;
                    dst += 128 - cur_x;
                } else {
                    for (int x = cur_x; x < llx1; x++) {
                        if ((*srct++ == tpri || (flags & DRAW_OPAQUE)) && !(m & (0x8000 >> (x >> 3)))) *dst = pen_tab_[*src];
                        src++;
                        dst++;
                    }
                }
            }
            llx -= 128;
            cur_x = 0;
        }
        source += 512;
        trans += 512;
        dest += dw_;
        mask += 4;
    }
}

// tilemap_t::draw with one scroll value: dest (x, y) takes pixmap
// ((x + sx) & 511, (y + sy) & 511) where (flags & mask) == value; mask is the
// category, plus layer 0 (opacity) unless drawing opaque.
void Video::tilemap_draw(std::vector<uint32_t> &dm, int L, int sx, int sy, int minx, int maxx, int miny, int maxy, int flags) {
    const uint8_t cat = uint8_t(flags & CATEGORY_MASK);
    const uint8_t mask = (flags & DRAW_OPAQUE) ? CATEGORY_MASK : uint8_t(CATEGORY_MASK | PIXEL_LAYER0);
    const uint8_t value = (flags & DRAW_OPAQUE) ? cat : uint8_t(cat | PIXEL_LAYER0);
    for (int y = std::max(miny, 0); y <= std::min(maxy, H - 1); y++)
        for (int x = std::max(minx, 0); x <= std::min(maxx, dw_ - 1); x++) {
            const size_t i = size_t((y + sy) & 511) * 512 + size_t((x + sx) & 511);
            if ((flags_[L][i] & mask) == value) dm[size_t(y) * size_t(dw_) + size_t(x)] = pen_tab_[pixmap_[L][i]];
        }
}

// segaic24 draw_common for the rgb32 bitmap, cliprect = the whole screen.
void Video::draw(std::vector<uint32_t> &bitmap, int layer, int flags) {
    uint16_t hscr = tile(0x5000 + uint32_t(layer >> 1));
    uint16_t vscr = tile(0x5004 + uint32_t(layer >> 1));
    const uint16_t ctrl = tile(0x5004 + uint32_t((layer >> 1) & 2));
    uint16_t mask[0x800];
    for (uint32_t i = 0; i < 0x800; i++) mask[i] = tile((layer & 4 ? 0x6800 : 0x6000) + i);
    const uint16_t tpri = uint16_t(layer & 1);
    layer >>= 1;
    const int fl = tpri | flags;

    if (vscr & 0x8000) return; // layer disable

    if (ctrl & 0x6000) { // special window/scroll modes
        if (layer & 1) return;
        const int sy = vscr & 0x1ff;
        if (hscr & 0x8000) {
            const uint32_t hscrtb = 0x4000 + 0x200 * uint32_t(layer);
            switch ((ctrl & 0x6000) >> 13) {
            case 1: {
                const uint16_t v = uint16_t((-vscr) & 0x1ff);
                if (!((-vscr) & 0x200)) layer ^= 1;
                for (int y = 0; y < H; y++) {
                    const int l1 = y >= v ? layer ^ 1 : layer;
                    const uint16_t h = tile(hscrtb + uint32_t(y)) & 0x1ff;
                    tilemap_draw(bitmap, l1, -h, sy, 0, dw_ - 1, y, y, fl);
                }
                break;
            }
            case 2:
            case 3:
                for (int y = 0; y < H; y++) {
                    hscr = tile(hscrtb + uint32_t(y));
                    const int h = hscr & 0x1ff;
                    int l1 = layer;
                    if (!(hscr & 0x200)) l1 ^= 1;
                    tilemap_draw(bitmap, l1, -h, sy, 0, std::min(dw_ - 1, h - 1), y, y, fl);
                    tilemap_draw(bitmap, l1 ^ 1, -h, sy, std::max(0, h), dw_ - 1, y, y, fl);
                }
                break;
            }
        } else {
            const int sx = -(hscr & 0x1ff);
            switch ((ctrl & 0x6000) >> 13) {
            case 1: {
                const int v = (-vscr) & 0x1ff;
                if (!((-vscr) & 0x200)) layer ^= 1;
                tilemap_draw(bitmap, layer, sx, sy, 0, dw_ - 1, 0, std::min(H - 1, v - 1), fl);
                tilemap_draw(bitmap, layer ^ 1, sx, sy, 0, dw_ - 1, std::max(0, v), H - 1, fl);
                break;
            }
            case 2:
            case 3: {
                const int h = hscr & 0x1ff;
                if (!(hscr & 0x200)) layer ^= 1;
                tilemap_draw(bitmap, layer, sx, sy, 0, std::min(dw_ - 1, h - 1), 0, H - 1, fl);
                tilemap_draw(bitmap, layer ^ 1, sx, sy, std::max(0, h), dw_ - 1, 0, H - 1, fl);
                break;
            }
            }
        }
        return;
    }

    const int win = layer & 1;
    if (hscr & 0x8000) {
        const uint32_t hscrtb = 0x4000 + 0x200 * uint32_t(layer);
        vscr &= 0x1ff;
        for (int y = 0; y < 384; y++) {
            hscr = uint16_t((-tile(hscrtb + uint32_t(y))) & 0x1ff);
            if (hscr + dw_ <= 512) {
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, vscr, 0, y, dw_, y + 1);
            } else {
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, vscr, 0, y, 512 - hscr, y + 1);
                draw_rect(bitmap, mask, tpri, flags, win, layer, 0, vscr, 512 - hscr, y, dw_, y + 1);
            }
            vscr = (vscr + 1) & 0x1ff;
        }
    } else {
        hscr = uint16_t((-hscr) & 0x1ff);
        vscr = uint16_t((+vscr) & 0x1ff);
        if (hscr + dw_ <= 512) {
            if (vscr + 384 <= 512) {
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, vscr, 0, 0, dw_, 384);
            } else {
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, vscr, 0, 0, dw_, 512 - vscr);
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, 0, 0, 512 - vscr, dw_, 384);
            }
        } else {
            if (vscr + 384 <= 512) {
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, vscr, 0, 0, 512 - hscr, 384);
                draw_rect(bitmap, mask, tpri, flags, win, layer, 0, vscr, 512 - hscr, 0, dw_, 384);
            } else {
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, vscr, 0, 0, 512 - hscr, 512 - vscr);
                draw_rect(bitmap, mask, tpri, flags, win, layer, 0, vscr, 512 - hscr, 0, dw_, 512 - vscr);
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, 0, 0, 512 - vscr, 512 - hscr, 384);
                draw_rect(bitmap, mask, tpri, flags, win, layer, 0, 0, 512 - hscr, 512 - vscr, dw_, 384);
            }
        }
    }
}

bool Video::system24_gpu_compatible() const {
    // The Vita GXM compositor supports normal windowing plus all three
    // System24 split-layer modes. Keep this query for the CPU fallback API.
    return true;
}

const std::vector<GeoPoly> &Video::gpu_polys() const {
    static const std::vector<GeoPoly> empty;
    return gpu_polys_ ? *gpu_polys_ : empty;
}

void Video::screen_update(const std::vector<GeoPoly> &polys, int windows, const VideoMem &mem) {
    // Threaded drawing only on the plain CPU path (see set_threaded).
    const bool threaded = threaded_ && !(margin_ && hud_edges_) && !external_3d_;
    VideoProfile drawn{};
    if (threaded_) {
        sync();
        drawn = job_profile_; // the previous update's drawing
        std::swap(screen_, display_);
    }
    gpu_polys_ = &polys;
    gpu_windows_ = windows;
    gpu_mem_ = mem;
    profile_ = {};
    uint64_t before = ticks();
    if (threaded) {
        // The emulation runs on while the worker draws: it reads this copy.
        std::memcpy(tile_snap_.data(), tile_ram_, tile_snap_.size());
        tile_src_ = tile_snap_.data();
    } else {
        tile_src_ = tile_ram_;
        pen_tab_ = pens_;
    }
    // Retain the reference's sticky palette-dirty behavior. palette_w marks
    // cached composition dirty only if the resulting RGB value really changed.
    if (palette_dirty_) {
        for (uint32_t i = 0; i < 0x1000; i++) palette_w(i, mem.palram, mem.colorxlat);
        palette_dirty_ = false;
    }
#ifdef M2_VITA_RENDER_OPT
    update_tile_cache();
#else
    decode_layers();
#endif
    if (system24_source_dirty_) {
        ++system24_texture_generation_;
        system24_source_dirty_ = false;
    }
    profile_.tile_cache = ticks() - before;
#ifndef M2_VITA_RENDER_OPT
    if (external_3d_ && desktop_) {
        // Desktop hardware renderer: it draws the tilemap layers from the
        // pixmaps, with this frame's registers and pens (the game may write
        // them again before the frame is drawn). The CPU decides what the
        // composition needs: how to fill the widescreen margins (the 3D
        // coverage, estimated from the polygons: no CPU 3D layer here) and
        // whether the HUD moves to the edges, which it does itself (the
        // front layers drawn here; the condition panel's polygons move on
        // the GPU, gpu_hud_shift).
        rendered_now_ = false;
        for (uint32_t i = 0; i < kGpuTileWords; ++i) gpu_tile_words_[i] = tile(kGpuTileFirst + i);
        std::copy_n(pens_, kGpuPens, gpu_pens_.data());
        if (margin_) coverage_ = polys.empty() ? 0 : raster_.coverage_estimate(polys, windows, crtc_x_ + margin_, crtc_y_);
        hud_on_ = margin_ && hud_edges_ && raster_.find_race_hud(polys, crtc_x_ + margin_, crtc_y_);
        if (hud_on_) {
            before = ticks();
            std::fill(sys24_.begin(), sys24_.end(), 0u);
            for (int layer = 3; layer >= 0; --layer) draw(sys24_, (layer << 1) | 1, 0);
            foreground_gpu_.assign(size_t(width()) * H, 0u);
            copy_front_hud_to_edges(foreground_gpu_);
            ++foreground_generation_;
            profile_.tile_draw += ticks() - before;
        }
        return;
    }
#endif
    if (external_3d_ && !desktop_ && system24_gpu_compatible()) {
        // GXM composes the cached System-24 tile textures around the 3D
        // layer. Do not spend ~35 ms rebuilding CPU bitmaps for scrolling.
        rendered_now_ = false;
        return;
    }
    Job job;
    job.polys = &polys;
    job.windows = windows;
    job.mem = mem;
#ifdef M2_VITA_RENDER_OPT
    job.background_dirty = background_dirty_;
    job.foreground_dirty = foreground_dirty_;
    background_dirty_ = foreground_dirty_ = false;
#endif
    if (!threaded) {
        compose(job, profile_);
        return;
    }
    job.threaded = true;
    job.crtc_x = crtc_x_;
    job.crtc_y = crtc_y_;
    job.render_x = render_x_;
    job.render_y = render_y_;
    std::copy_n(pens_, 8192, pens_snap_.data());
    pen_tab_ = pens_snap_.data();
    job.draw_3d = !render_done_ && !polys.empty();
    if (job.draw_3d) {
        render_done_ = true;
        polys_snap_.assign(polys.begin(), polys.end());
        job.polys = &polys_snap_;
        std::memcpy(palram_snap_.data(), mem.palram, palram_snap_.size());
        std::memcpy(xlat_snap_.data(), mem.colorxlat, xlat_snap_.size());
        std::memcpy(luma_snap_.data(), mem.lumaram, luma_snap_.size());
        job.mem.palram = palram_snap_.data();
        job.mem.colorxlat = xlat_snap_.data();
        job.mem.lumaram = luma_snap_.data();
    }
    job.have_3d = render_done_;
    // Report this update's decode with the previous update's drawing.
    profile_.tile_draw = drawn.tile_draw;
    profile_.raster = drawn.raster;
    profile_.composite = drawn.composite;
    profile_.layers_rebuilt = drawn.layers_rebuilt;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job_ = job;
        job_profile_ = {};
        job_pending_ = true;
    }
    cv_.notify_all();
}

// The drawing half of screen_update: 2D back layers, the 3D layer, 2D front
// layers into screen_. Threaded, it runs on the worker and reads only the
// job and the snapshots.
void Video::compose(const Job &job, VideoProfile &profile) {
    const std::vector<GeoPoly> &polys = *job.polys;
    const int windows = job.windows;
    const VideoMem &mem = job.mem;
    uint64_t before = 0;
    // Non-zero pixels of a `width`-wide source onto the screen at column `at`.
    const size_t out_w = size_t(width());
    auto copy_trans = [&](const uint32_t *source, size_t stride, int width = W, int at = 0) {
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < width; ++x)
                if (const uint32_t pixel = source[size_t(y) * stride + size_t(x)])
                    screen_[size_t(y) * out_w + size_t(at + x)] = pixel;
    };
#ifdef M2_VITA_RENDER_OPT
    // The 2D layers. With threaded 3D, drawn while the raster workers start
    // on the polygons (separate buffers), else now.
    auto tiles = [&]() {
    before = ticks();
    if (job.background_dirty) {
        // All tile writes are replacements, not blends. Drawing the back
        // layers over pen 0 is identical to zero + transparent copy over pen 0.
        draw_back_layers(background_, wrap_back() ? 512 : W);
        ++background_generation_;
        profile.layers_rebuilt = true;
    }
    if (job.foreground_dirty) {
        std::fill(sys24_.begin(), sys24_.end(), 0u);
        for (int layer = 3; layer >= 0; --layer) draw(sys24_, (layer << 1) | 1, 0);
        ++foreground_generation_;
        profile.layers_rebuilt = true;
    }
    profile.tile_draw = ticks() - before;
    before = ticks();
    if (wrap_back()) {
        copy_back_wrapped(background_.data());
    } else if (margin_) { // widescreen: the 496-wide back layers in the centre
        for (int y = 0; y < H; ++y)
            std::copy_n(background_.data() + size_t(y) * W, W, screen_.data() + size_t(y) * out_w + size_t(margin_));
    } else {
        std::copy_n(background_.data(), screen_.size(), screen_.data());
    }
    profile.composite += ticks() - before;
    };
    const bool tiles_alongside = job.threaded && job.draw_3d && !external_3d_ && !(margin_ && hud_edges_);
    if (!tiles_alongside) tiles();
#else
    before = ticks();
    if (wrap_back()) {
        draw_back_layers(back512_, 512);
        profile.tile_draw += ticks() - before;
        profile.layers_rebuilt = true;
        before = ticks();
        copy_back_wrapped(back512_.data());
        profile.composite += ticks() - before;
    } else {
        std::fill(screen_.begin(), screen_.end(), pen_tab_[0]);
        std::fill(sys24_.begin(), sys24_.end(), 0u);
        for (int layer = 3; layer >= 2; --layer) draw(sys24_, layer << 1, DRAW_OPAQUE);
        for (int layer = 1; layer >= 0; --layer) draw(sys24_, layer << 1, 0);
        profile.tile_draw += ticks() - before;
        profile.layers_rebuilt = true;
        before = ticks();
        copy_trans(sys24_.data(), W, W, margin_);
        profile.composite += ticks() - before;
    }
#endif
    rendered_now_ = false;
    if (external_3d_) {
        // Save the exact two System-24 layers separately. The Vita frontend
        // draws background -> GPU 3D -> foreground. No CPU polygon pixels are
        // produced in this mode, so raster_ms should remain zero.
        std::copy_n(screen_.data(), screen_.size(), background_gpu_.data());
#ifndef M2_VITA_RENDER_OPT
        // Reference path has not drawn the post-3D tile pass yet.
        before = ticks();
        std::fill(sys24_.begin(), sys24_.end(), 0u);
        for (int layer = 3; layer >= 0; --layer) draw(sys24_, (layer << 1) | 1, 0);
        profile.tile_draw += ticks() - before;
#endif
        std::fill(foreground_gpu_.begin(), foreground_gpu_.end(), 0u);
        std::copy_n(sys24_.data(), std::min(sys24_.size(), foreground_gpu_.size()), foreground_gpu_.data());
        return;
    }
    // Widescreen, HUD at the edges: the front tilemaps are drawn first, to
    // decide which HUD groups move; 3D windows inside a moved group go with it.
    const bool hud_edges = margin_ && hud_edges_;
    if (hud_edges) {
        before = ticks();
        std::fill(sys24_.begin(), sys24_.end(), 0u);
        for (int layer = 3; layer >= 0; --layer) draw(sys24_, (layer << 1) | 1, 0);
        profile.tile_draw += ticks() - before;
        // Only while the race HUD is on screen (its condition panel's box).
        const bool race_hud = raster_.find_race_hud(polys, crtc_x_ + margin_, crtc_y_);
        if (race_hud != hud_on_) { hud_on_ = race_hud; set_raster_hud_moves(); render_done_ = false; }
    }
    if (job.threaded) {
        // Widescreen (without the HUD at the edges) works here too: the
        // margin only changes between updates, after a sync.
        if (job.draw_3d) {
#ifdef M2_VITA_RENDER_OPT
            const uint64_t start = ticks();
            raster_.render(polys, windows, mem, job.crtc_x + margin_, job.crtc_y, job.render_x + margin_, job.render_y,
                           0, width() - 1, 0, H - 1, tiles_alongside ? std::function<void()>(tiles) : std::function<void()>());
            const uint64_t elapsed = ticks() - start, tile_part = profile.tile_draw + profile.composite;
            profile.raster = elapsed > tile_part ? elapsed - tile_part : 0; // the 3D's share
#else
            before = ticks();
            raster_.render(polys, windows, mem, job.crtc_x + margin_, job.crtc_y, job.render_x + margin_, job.render_y,
                           0, width() - 1, 0, H - 1);
            profile.raster = ticks() - before;
#endif
            if (margin_) coverage_ = raster_coverage();
            rendered_now_ = true;
        }
#ifndef M2_VITA_RENDER_OPT
        before = ticks();
        std::fill(sys24_.begin(), sys24_.end(), 0u);
        for (int layer = 3; layer >= 0; --layer) draw(sys24_, (layer << 1) | 1, 0);
        profile.tile_draw += ticks() - before;
#endif
        before = ticks();
        if (margin_ && !wrap_back()) {
            if (!job.have_3d) coverage_ = 0;
            fill_margins();
        }
        if (margin_ && pillarbox_2d_ && !(job.have_3d && raster_in_margins())) {
            black_margins();
            if (job.have_3d) copy_trans(raster_.pixels() + margin_, size_t(raster_.stride()), W, margin_);
        } else if (job.have_3d) {
            copy_trans(raster_.pixels(), size_t(raster_.stride()), width());
        }
        copy_trans(sys24_.data(), W, W, margin_);
        profile.composite += ticks() - before;
        return;
    }
    if (!render_done_ && !polys.empty()) {
        before = ticks();
        raster_.render(polys, windows, mem, crtc_x_ + margin_, crtc_y_, render_x_ + margin_, render_y_, 0,
                       width() - 1, 0, H - 1);
        profile.raster = ticks() - before;
        if (margin_) coverage_ = raster_coverage();
        render_done_ = true;
        rendered_now_ = true;
    }
    before = ticks();
    if (margin_ && !wrap_back()) {
        if (!render_done_) coverage_ = 0; // no 3D this frame: a 2D screen
        fill_margins();
    }
    if (margin_ && pillarbox_2d_ && !(render_done_ && raster_in_margins())) {
        black_margins();
        if (render_done_) copy_trans(raster_.pixels() + margin_, size_t(raster_.stride()), W, margin_);
    } else if (render_done_) {
        copy_trans(raster_.pixels(), size_t(raster_.stride()), width());
    }
    profile.composite += ticks() - before;
#ifndef M2_VITA_RENDER_OPT
    if (!hud_edges) {
        before = ticks();
        std::fill(sys24_.begin(), sys24_.end(), 0u);
        for (int layer = 3; layer >= 0; --layer) draw(sys24_, (layer << 1) | 1, 0);
        profile.tile_draw += ticks() - before;
    }
#endif
    before = ticks();
    if (hud_edges && hud_on_) {
        copy_front_hud_to_edges(screen_);
    } else {
        copy_trans(sys24_.data(), W, W, margin_);
    }
    profile.composite += ticks() - before;
}

// Widescreen, HUD at the edges: the race HUD's side groups (lap and lap
// times at the top left; position, condition panel and course map on the
// right) move out by the margin; the centre (speed, speedometer) stays.
// The front tilemaps also carry banners that scroll through those areas
// ("ROLLING START"), so what moves is decided per item, not per area: the
// front layers' pixels are grouped into blobs (pixels within kHudJoin of
// each other join, so a banner's letters and backing are one blob), and a
// blob moves only if it lies wholly inside a group. A banner crossing a
// group's edge stays put and is never torn, and nothing flips frame to
// frame as its letters pass. Rectangles in 496-wide coordinates.
namespace {
struct HudGroup { int x0, x1, y0, y1, side; };
constexpr HudGroup kHudGroups[2] = {{0, 125, 0, 130, -1},   // lap, lap times
                                    {352, 496, 0, 300, 1}}; // position ("40TH" reaches x 367), condition, course map
constexpr int kHudJoin = 4;
} // namespace

void Video::set_raster_hud_moves() {
    // The condition panel's overlay quads go with the right-hand group.
    raster_.set_hud_shift(hud_on_ ? kHudGroups[1].side * margin_ : 0);
}

void Video::copy_front_hud_to_edges(std::vector<uint32_t> &out) {
    const size_t n = size_t(W) * H;
    // Pixels present, widened by kHudJoin in x then y (a square neighbourhood).
    hud_mask_.assign(n, 0);
    hud_tmp_.assign(n, 0);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (sys24_[size_t(y) * W + size_t(x)])
                for (int d = std::max(0, x - kHudJoin); d <= std::min(W - 1, x + kHudJoin); ++d)
                    hud_tmp_[size_t(y) * W + size_t(d)] = 1;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (hud_tmp_[size_t(y) * W + size_t(x)])
                for (int d = std::max(0, y - kHudJoin); d <= std::min(H - 1, y + kHudJoin); ++d)
                    hud_mask_[size_t(d) * W + size_t(x)] = 1;
    // Label the widened blobs; bound each by its real pixels.
    hud_label_.assign(n, -1);
    hud_box_.clear();
    std::vector<uint32_t> &stack = hud_stack_;
    for (size_t start = 0; start < n; ++start) {
        if (!hud_mask_[start] || hud_label_[start] >= 0) continue;
        const int label = int(hud_box_.size());
        hud_box_.push_back({W, 0, H, 0});
        stack.assign(1, uint32_t(start));
        hud_label_[start] = label;
        while (!stack.empty()) {
            const uint32_t i = stack.back();
            stack.pop_back();
            const int x = int(i % W), y = int(i / W);
            if (sys24_[i]) {
                auto &b = hud_box_[size_t(label)];
                b[0] = std::min(b[0], x), b[1] = std::max(b[1], x + 1);
                b[2] = std::min(b[2], y), b[3] = std::max(b[3], y + 1);
            }
            const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
            for (int k = 0; k < 4; ++k) {
                if (nx[k] < 0 || nx[k] >= W || ny[k] < 0 || ny[k] >= H) continue;
                const uint32_t j = uint32_t(ny[k]) * W + uint32_t(nx[k]);
                if (hud_mask_[j] && hud_label_[j] < 0) hud_label_[j] = label, stack.push_back(j);
            }
        }
    }
    // Each blob's move: its group's, if it lies wholly inside one.
    hud_move_.assign(hud_box_.size(), 0);
    for (size_t l = 0; l < hud_box_.size(); ++l)
        for (const HudGroup &G : kHudGroups) {
            const auto &b = hud_box_[l];
            if (b[0] >= G.x0 && b[1] <= G.x1 && b[2] >= G.y0 && b[3] <= G.y1) hud_move_[l] = G.side * margin_;
        }
    const size_t out_w = size_t(width());
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const size_t i = size_t(y) * W + size_t(x);
            if (const uint32_t pixel = sys24_[i])
                out[size_t(y) * out_w + size_t(margin_ + x + hud_move_[size_t(hud_label_[i])])] = pixel;
        }
}

// Widescreen side margins, under the 3D layer. On a 2D screen (car and
// circuit select, titles: see scene()) each row carries its own edge colours
// out: the art covers only 496 columns. Behind a 3D scene (the race, the
// attract's camera shots) the margins are the sky's plain colour (the back layers'
// top-left pixel, open sky), or with "stretch tile background" the backdrop
// as drawn for the 496 columns is stretched across the whole width, never
// repeated: the race sky is one 512-pixel layer whose ends do not meet, so
// drawing it further (tried, also with split pairs) showed a seam.
// Pen 0, then the back tilemaps (opaque layers 3 and 2, then 1 and 0) into a
// `width`-wide bitmap: W, or 512 (the tilemaps' full width) for wrap_back.
void Video::draw_back_layers(std::vector<uint32_t> &bitmap, int width) {
    if (bitmap.size() < size_t(width) * (H + 4)) bitmap.resize(size_t(width) * (H + 4));
    std::fill(bitmap.begin(), bitmap.begin() + ptrdiff_t(size_t(width) * H), pen_tab_[0]);
    dw_ = width;
    for (int layer = 3; layer >= 2; --layer) draw(bitmap, layer << 1, DRAW_OPAQUE);
    for (int layer = 1; layer >= 0; --layer) draw(bitmap, layer << 1, 0);
    dw_ = W;
}

// Widescreen with wrap_back: screen column x (0 = the left margin's edge)
// shows tilemap column (x - margin) mod 512 of the 512-wide back layers, as
// the hardware's scrolling would with a wider screen.
void Video::copy_back_wrapped(const uint32_t *src) {
    const size_t out_w = size_t(width());
    for (int y = 0; y < H; ++y) {
        const uint32_t *row = src + size_t(y) * 512;
        uint32_t *out = screen_.data() + size_t(y) * out_w;
        for (int x = 0; x < int(out_w); ++x) out[x] = row[(x - margin_) & 511];
    }
}

// Widescreen: does the 3D layer reach into the side margins? (Not on menu
// screens, whose few polygons stay inside the original screen.)
bool Video::raster_in_margins() const {
    for (int y = 0; y < H; ++y) {
        const uint32_t *row = raster_.pixels() + size_t(y) * size_t(raster_.stride());
        for (int x = 0; x < margin_; ++x)
            if (row[x] | row[margin_ + W + x]) return true;
    }
    return false;
}

// Widescreen, pillarbox_2d: black margins (a frame with no 3D in them).
void Video::black_margins() {
    const size_t out = size_t(width());
    for (int y = 0; y < H; ++y) {
        uint32_t *row = &screen_[size_t(y) * out];
        std::fill(row, row + margin_, rgb(0, 0, 0));
        std::fill(row + margin_ + W, row + out, rgb(0, 0, 0));
    }
}

// Widescreen: how much of the original screen the 3D layer covers (%).
int Video::raster_coverage() const {
    size_t covered = 0;
    for (int y = 0; y < H; ++y) {
        const uint32_t *row = raster_.pixels() + size_t(y) * size_t(raster_.stride()) + size_t(margin_);
        for (int x = 0; x < W; ++x) covered += row[x] != 0;
    }
    return int(covered * 100 / (size_t(W) * H));
}

void Video::fill_margins() {
    const bool scene = this->scene();
    const int out = width();
    const uint32_t sky = screen_[size_t(margin_)];
    if (scene && stretch_backdrop_) {
        stretch_row_.resize(size_t(W) + 1);
        if (stretch_map_.size() != size_t(out)) {
            // out column x samples backdrop column (x + 0.5) * W / out - 0.5, blended
            stretch_map_.resize(size_t(out));
            for (int x = 0; x < out; ++x) {
                const float u = std::clamp((float(x) + 0.5f) * float(W) / float(out) - 0.5f, 0.0f, float(W - 1));
                const int i = int(u);
                stretch_map_[size_t(x)] = uint32_t(i) << 9 | uint32_t((u - float(i)) * 256.0f + 0.5f);
            }
        }
        for (int y = 0; y < H; ++y) {
            uint32_t *row = &screen_[size_t(y) * size_t(out)];
            std::copy_n(row + margin_, W, stretch_row_.data());
            stretch_row_[size_t(W)] = stretch_row_[size_t(W - 1)]; // column W-1 blends with itself
            for (int x = 0; x < out; ++x) {
                const uint32_t m = stretch_map_[size_t(x)], i = m >> 9, f = m & 0x1ff, g = 256 - f;
                const uint32_t a = stretch_row_[i], b = stretch_row_[i + 1];
                const uint32_t rb = (((a & 0xff00ffu) * g + (b & 0xff00ffu) * f + 0x800080u) >> 8) & 0xff00ffu;
                const uint32_t gc = (((a & 0x00ff00u) * g + (b & 0x00ff00u) * f + 0x008000u) >> 8) & 0x00ff00u;
                row[x] = rb | gc | (a & 0xff000000u);
            }
        }
        return;
    }
    for (int y = 0; y < H; ++y) {
        uint32_t *row = &screen_[size_t(y) * size_t(out)];
        const uint32_t left = scene ? sky : row[margin_], right = scene ? sky : row[margin_ + W - 1];
        std::fill(row, row + margin_, left ? left : pen_tab_[0]);
        std::fill(row + margin_ + W, row + out, right ? right : pen_tab_[0]);
    }
}

void Video::set_wide_margin(int margin) {
#if defined(M2_VITA_RENDER_OPT) && !defined(__SWITCH__)
    margin = 0; // the Vita compositor draws the 496-wide layers itself
#endif
    if (external_3d_ && !desktop_) margin = 0;
    margin = std::max(margin, 0);
    if (margin == margin_) return;
    if (threaded_) sync(); // the worker draws at the old width
    margin_ = margin;
    set_raster_hud_moves();
    screen_.assign(size_t(width()) * H, 0u);
    if (threaded_) display_ = screen_;
#ifdef M2_VITA_RENDER_OPT
    background_dirty_ = true; // the cached back layers, at the new width
#endif
    raster_.set_wide_margin(margin_);
    render_done_ = false; // redraw the 3D layer at the new width
}

uint64_t Video::screen_hash() const {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (uint32_t px : screen())
        for (int b = 0; b < 4; b++) {
            h ^= (px >> (8 * b)) & 0xff;
            h *= 0x100000001b3ULL;
        }
    return h;
}

} // namespace rt
