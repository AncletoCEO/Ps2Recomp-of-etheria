#include "game_overrides.h"
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include <sstream>
#include "ps2_runtime_calls.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "ps2_log.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <vector>
#include "sword_etheria_return_stubs.h"

namespace
{
    // 2026-10-07g: wrap del original de 6e4848 (file-scope: las lambdas de
    // replaceFunction no capturan; el puntero se guarda antes de reemplazar).
    PS2Runtime::RecompiledFunction g_orig4848 = nullptr;
    // 2026-10-07s (perfil 50Hz, REVERSIBLE): wrap del handler VBlank
    // (invocado vía scheduler → SÍ dispara). Cada 600 vblanks (~12s)
    // vuelca snapshot de threads EE (pc/estado) + eeCycle.
    PS2Runtime::RecompiledFunction g_orig6e45d8 = nullptr;
    // 2026-10-10 fix: máximo destino del loop que arma el burst (0xC50000-0xDB0000).
    uint32_t g_burstMaxDst = 0u;
    // 2026-10-09 (Fase 0): probes del init/exit de SIF del juego.
    PS2Runtime::RecompiledFunction g_origCf158 = nullptr;
    PS2Runtime::RecompiledFunction g_origD6380 = nullptr;
    PS2Runtime::RecompiledFunction g_origCf360 = nullptr;
    PS2Runtime::RecompiledFunction g_origCfc60 = nullptr;
    // 2026-10-08 TRIAGE: init del motor de DMA (¿corre en runtime?).
    PS2Runtime::RecompiledFunction g_orig6e68e0 = nullptr;
    PS2Runtime::RecompiledFunction g_orig6e6934 = nullptr;
    PS2Runtime::RecompiledFunction g_orig6e6640 = nullptr;
    PS2Runtime::RecompiledFunction g_orig4d1a60 = nullptr;
    PS2Runtime::RecompiledFunction g_orig6efd28 = nullptr;
    PS2Runtime::RecompiledFunction g_orig50f750 = nullptr;
    void triageLoadGs(PS2Runtime *runtime);
    void triage6e45d8wrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static int s_vb = 0;

        if (++s_vb % 60 == 1) {
            const EeKernelSnapshot snap = runtime->eeScheduler().snapshot();
            std::ostringstream oss;
            oss << "[triage-vbprof] n=" << s_vb << " eeCycle=" << snap.eeCycle << " run=" << snap.runningThreadId;
            for (const EeThreadSnapshot &t : snap.threads) {
                oss << " [id=" << t.id << " pc=0x" << std::hex << t.pc << " ra=0x" << t.ra << std::dec << " st=" << static_cast<int>(t.status) << " pri=" << t.currentPriority << " wr=" << static_cast<int>(t.waitReason) << " wid=" << t.waitId << "]";
            }
            oss << " sems={";
            for (const EeSemaphoreSnapshot &s : snap.semaphores) {
                if (s.waiters != 0 || s.count <= 0) oss << s.id << ":" << s.count << "/" << s.maxCount << "w" << s.waiters << " ";
            }
            oss << "} evf={";
            for (const EeEventFlagSnapshot &e : snap.eventFlags) {
                if (e.waiters != 0) oss << e.id << ":0x" << std::hex << e.bits << std::dec << "w" << e.waiters << " ";
            }
            oss << "}";
            {
                // 2026-10-10 fix: estado del gate de carga (gp=0xA28070): +0x50 (hilo a despertar),
                // +0x54 (precondición del setter) y +0x58 (flag de carga completa que espera el loader).
                const uint32_t gp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[28]));
                auto rd = [&](uint32_t off) -> uint32_t {
                    uint32_t v = 0u;
                    const uint32_t a = (gp + off) & 0x01FFFFFFu;
                    if (rdram != nullptr && a + 4u <= 0x02000000u) std::memcpy(&v, rdram + a, 4u);
                    return v;
                };
                oss << " gp50=0x" << std::hex << rd(0x50u) << " gp54=0x" << rd(0x54u)
                    << " gp58=0x" << rd(0x58u) << " gp80=0x" << rd(0x80u)
                    << " gp5cc=0x" << rd(0x5CCu) << " gp5d0=0x" << rd(0x5D0u)
                    << " gp740=0x" << rd(0x740u) << " gp744=0x" << rd(0x744u) << std::dec;
            }
            {
                // 2026-10-10 fix: ¿el recomp llena la región del "burst" 0xC80000-0xDB0000?
                // El real la llena hasta 0xDB; el recomp se corta en 0xCF0000. Con SWORD_SEED_TAIL
                // se siembra 0xDEADBEEF en la cola 0xCF0000-0xDB0000 para ver si algo la escribe.
                uint32_t burstNz = 0u, firstNz = 0u, tailMarker = 0u, tailNz = 0u;
                if (rdram != nullptr) {
                    for (uint32_t a = 0x00C80000u; a < 0x00DB0000u; a += 4u) {
                        uint32_t v = 0u;
                        std::memcpy(&v, rdram + a, 4u);
                        if (v != 0u) { ++burstNz; if (firstNz == 0u) firstNz = a; }
                        if (a >= 0x00CF0000u) {
                            if (v != 0u) ++tailNz;
                            if (v == 0xDEADBEEFu) ++tailMarker;
                        }
                    }
                }
                oss << " burst_nz=" << burstNz << " burst_first=0x" << std::hex << firstNz
                    << std::dec << " tail_marker=" << tailMarker << " tail_nz=" << tailNz
                    << " burst_maxdst=0x" << std::hex << g_burstMaxDst << std::dec;
            }
            // 2026-10-10 fix: sembrar marcador en la cola (una vez). Env SWORD_SEED_TAIL=1.
            if (std::getenv("SWORD_SEED_TAIL") != nullptr) {
                static bool s_tailSeeded = false;
                if (!s_tailSeeded && s_vb >= 100) {
                    s_tailSeeded = true;
                    if (rdram != nullptr) {
                        const uint32_t k = 0xDEADBEEFu;
                        for (uint32_t a = 0x00CF0000u; a < 0x00DB0000u; a += 4u) std::memcpy(rdram + a, &k, 4u);
                    }
                    RUNTIME_LOG("[seed-tail] marcador 0xCF0000-0xDB0000 vb=" << s_vb);
                }
            }
            // 2026-10-10 fix: dump único de la RAM post-load (para diffear el burst vs s02).
            // Env: SWORD_DUMP_LATE=1.
            if (std::getenv("SWORD_DUMP_LATE") != nullptr) {
                static bool s_lateDumped = false;
                if (!s_lateDumped && s_vb >= 4000) {
                    s_lateDumped = true;
                    std::ofstream out("/tmp/sword_recomp_late.bin", std::ios::binary);
                    if (out) out.write(reinterpret_cast<const char *>(rdram), 0x02000000u);
                    RUNTIME_LOG("[dump-late] vb=" << s_vb << " -> /tmp/sword_recomp_late.bin");
                }
            }
            RUNTIME_LOG(oss.str());
        }
        static int s_shotVb = 0;
        if (++s_shotVb % 120 == 1) {
            std::vector<uint8_t> rgba;
            uint32_t w = 0, h = 0;
            runtime->gs().latchHostPresentationFrame();
            if (runtime->gs().copyLatchedHostPresentationFrame(rgba, w, h) && !rgba.empty()) {
                uint32_t nonZeroRgb = 0;
                for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
                    if (rgba[i] != 0 || rgba[i + 1] != 0 || rgba[i + 2] != 0) {
                        ++nonZeroRgb;
                    }
                }
                char fn[128];
                std::snprintf(fn, sizeof(fn), "/tmp/opencode/shot_vb_%d.bmp", s_shotVb);
                std::ofstream out(fn, std::ios::binary);
                if (out) {
                    const uint32_t rowPad = (4 - (w * 3) % 4) % 4;
                    const uint32_t imgSize = (w * 3 + rowPad) * h;
                    const uint32_t fileSize = 54 + imgSize;
                    uint8_t hdr[54] = {};
                    hdr[0] = 'B'; hdr[1] = 'M';
                    std::memcpy(hdr + 2, &fileSize, 4);
                    const uint32_t off = 54;
                    std::memcpy(hdr + 10, &off, 4);
                    const uint32_t dib = 40;
                    std::memcpy(hdr + 14, &dib, 4);
                    std::memcpy(hdr + 18, &w, 4);
                    std::memcpy(hdr + 22, &h, 4);
                    const uint16_t planes = 1, bpp = 24;
                    std::memcpy(hdr + 26, &planes, 2);
                    std::memcpy(hdr + 28, &bpp, 2);
                    out.write(reinterpret_cast<char*>(hdr), 54);
                    std::vector<uint8_t> row(w * 3 + rowPad);
                    for (int y = static_cast<int>(h) - 1; y >= 0; --y) {
                        for (uint32_t x = 0; x < w; ++x) {
                            row[x * 3 + 0] = rgba[(y * w + x) * 4 + 2];
                            row[x * 3 + 1] = rgba[(y * w + x) * 4 + 1];
                            row[x * 3 + 2] = rgba[(y * w + x) * 4 + 0];
                        }
                        out.write(reinterpret_cast<char*>(row.data()), row.size());
                    }
                    RUNTIME_LOG("[shot-vb] n=" << s_shotVb << " saved " << fn << " " << w << "x" << h << " nonZeroRgb=" << nonZeroRgb << "/" << (w * h));
                }
            }
        }
        // 2026-10-09 (a, Fase 1): watchpoint por muestreo de páginas (4 KB).
        // Cada muestra loguea las páginas de EE RAM cuyo hash CAMBIÓ respecto
        // de la anterior: mapa de qué regiones están vivas/quietas. A/B:
        // SWORD_SCENE_WP=0. Dump crudo opcional con SWORD_RAM_DUMP=1.
        {
            static const bool s_sceneWp = []() {
                const char *e = std::getenv("SWORD_SCENE_WP");
                return e != nullptr && *e != '0';
            }();
            static const bool s_ramDump = std::getenv("SWORD_RAM_DUMP") != nullptr;
            static int s_sample = 0;
            static std::vector<uint32_t> s_prev;
            static bool s_init = false;
            constexpr uint32_t kPage = 4096u;
            constexpr uint32_t kBase = 0x00100000u;
            constexpr uint32_t kPages = (0x02000000u - kBase) / kPage;
            if (s_sceneWp) {
                ++s_sample;
                if (s_prev.size() != kPages) { s_prev.assign(kPages, 0u); s_init = false; }
                uint32_t changed = 0;
                char line[320];
                int n = 0;
                for (uint32_t p = 0; p < kPages; ++p) {
                    const uint8_t *base = rdram + kBase + p * kPage;
                    uint32_t h = 2166136261u;
                    for (uint32_t i = 0; i < kPage; i += 16u) {
                        h ^= *reinterpret_cast<const uint32_t *>(base + i);
                        h *= 16777619u;
                    }
                    if (s_init && h != s_prev[p]) {
                        ++changed;
                        if (n < (int)sizeof(line) - 16) {
                            n += std::snprintf(line + n, sizeof(line) - n, " %x", (kBase + p * kPage));
                        }
                    }
                    s_prev[p] = h;
                }
                if (s_init && changed != 0 && (s_sample % 5 == 0)) {
                    RUNTIME_LOG("[scene-pages] s=" << s_sample << " changed=" << changed << ":" << line);
                }
                s_init = true;
            }
            if (s_ramDump && s_sample % 6 == 0) {
                static int s_dumpN = 0;
                if (s_dumpN < 8) {
                    char fn[128];
                    std::snprintf(fn, sizeof(fn), "/tmp/sword_ram_%d.bin", s_dumpN);
                    std::ofstream out(fn, std::ios::binary);
                    if (out) {
                        out.write(reinterpret_cast<const char *>(rdram), 0x02000000u);
                    }
                    RUNTIME_LOG("[ram-dump] #" << s_dumpN << " -> " << fn << " (vb=" << s_vb << ")");
                    ++s_dumpN;
                }
            }
        }
        // 2026-10-09 (Fase 0): TEST — sembrar los flags que el real tiene y
        // nosotros no (SIF-init). Si el juego avanza, eran el gate.
        // Env: SWORD_SEED_REAL=1. Re-aplica cada ~5 s.
        if (std::getenv("SWORD_SEED_REAL") != nullptr && (s_vb % 300) == 0) {
            auto put = [&](uint32_t a, uint32_t v) {
                const uint32_t p = a & 0x01FFFFFFu;
                if (p + 4u <= 0x02000000u) std::memcpy(rdram + p, &v, 4);
            };
            put(0x0872098u, 0x22u);
            put(0x0873D78u, 1u);
            put(0x08720C0u, 1u);
            put(0x0873C04u, 7u);
            put(0x0873C08u, 7u);
            static int s_seed = 0;
            if (++s_seed <= 3) RUNTIME_LOG("[seed-real] applied at vb=" << s_vb << " n=" << s_seed);
        }
        // 2026-10-09 (gate del loader): el hilo de carga (recomp id=3, 0x628688)
        // hace `while (*(gp+0x58)==0) SleepThread()` (syscall 0x32). En el real se
        // pone gp+0x58=1 (0xA280C8) a ~3.5 s tras ss01 (PINE) y el hilo se despierta;
        // el recomp nunca lo hace → el loader duerme y no carga los `ptn_*`.
        // TRIAGE: setear el flag + despertar el hilo. Env SWORD_FORCE_LOADER=1.
        if (std::getenv("SWORD_FORCE_LOADER") != nullptr) {
            const uint32_t one = 1u;
            std::memcpy(rdram + (0x00A280C8u & 0x01FFFFFFu), &one, 4);
            (void)runtime->eeScheduler().wakeupThread(3, true);
        }
        // 2026-10-10 TRIAGE "forzar visual": teleportar el guest al estado post-burst
        // cargando la RAM cruda de un savestate PCSX2 (32 MB) UNA sola vez. Env
        // SWORD_LOAD_RDRAM=<eeMemory.bin>. El código recompilado es nativo, así que
        // sólo se reemplaza DATA guest; luego se marcan los flags de carga y se
        // despiertan los hilos 2/3. NO es fix real.
        if (const char *ramPath = std::getenv("SWORD_LOAD_RDRAM"); ramPath != nullptr && *ramPath != '\0') {
            static bool s_ramDone = false;
            if (!s_ramDone && s_vb >= 300) {
                s_ramDone = true;
                if (std::FILE *fp = std::fopen(ramPath, "rb"); fp != nullptr) {
                    static std::vector<char> s_buf;
                    s_buf.assign(0x02000000u, 0);
                    const size_t n = std::fread(s_buf.data(), 1u, s_buf.size(), fp);
                    std::fclose(fp);
                    // Inyectar sólo las regiones que DIFIEREN de s01 (≈recomp), leídas de
                    // <file>.ranges (líneas "off len" en hex). Así no se pisan los stacks del
                    // hilo (arriba de 0x1FED...). Sin archivo de rangos: copia los 32 MB.
                    if (n >= 0x02000000u) {
                        char rf[512];
                        std::snprintf(rf, sizeof(rf), "%s.ranges", ramPath);
                        std::FILE *rp = std::fopen(rf, "r");
                        uint32_t copied = 0u;
                        if (rp != nullptr) {
                            unsigned off = 0u, len = 0u;
                            while (std::fscanf(rp, "%x %x", &off, &len) == 2) {
                                if (off + len <= 0x02000000u) {
                                    std::memcpy(rdram + off, s_buf.data() + off, len);
                                    copied += len;
                                }
                            }
                            std::fclose(rp);
                        } else {
                            std::memcpy(rdram, s_buf.data(), 0x02000000u);
                            copied = 0x02000000u;
                        }
                        RUNTIME_LOG("[load-rdram] rangos copiados=" << copied);
                    }
                    const uint32_t one = 1u;
                    std::memcpy(rdram + (0x00A280C4u & 0x01FFFFFFu), &one, 4);
                    std::memcpy(rdram + (0x00A280C8u & 0x01FFFFFFu), &one, 4);
                    (void)runtime->eeScheduler().wakeupThread(2, true);
                    (void)runtime->eeScheduler().wakeupThread(3, true);
                    RUNTIME_LOG("[load-rdram] " << ramPath << " bytes=" << n << " vb=" << s_vb);
                } else {
                    RUNTIME_LOG("[load-rdram] FAIL open " << ramPath);
                }
            }
        }
        if (s_vb >= 300)
            triageLoadGs(runtime);
        // SWORD_FIX_VT=1: rescate de la vtable del objeto de escena 0xA887B8 (slots 0x108..0x11C).
        // El juego los deja en 0 y hace JALR a NULL (0x6d2684 -> +0x10C, 0x6d7090 -> +0x11C).
        // Escribe los defaults de FUN_006cdcf0 (+0x108..+0x114) y los de +0x118/+0x11C del real.
        if (std::getenv("SWORD_FIX_VT") != nullptr) {
            static const struct { uint32_t off; uint32_t val; } kVt[] = {
                {0x108u, 0x006CE228u}, {0x10Cu, 0x004C2D60u}, {0x110u, 0x006CE248u},
                {0x114u, 0x006CE268u}, {0x118u, 0x006D5F90u}, {0x11Cu, 0x006D5FB8u},
            };
            for (const auto &e : kVt) {
                const uint32_t a = (0x00A887B8u + e.off) & 0x01FFFFFFu;
                uint32_t v = 0u;
                std::memcpy(&v, rdram + a, 4u);
                if (v == 0u) {
                    std::memcpy(rdram + a, &e.val, 4u);
                    static int s_n = 0;
                    if (++s_n <= 6) RUNTIME_LOG("[fix-vt] 0xA887B8+0x" << std::hex << e.off << " := 0x" << e.val << std::dec);
                }
            }
        }
        if (g_orig6e45d8 != nullptr) {
            g_orig6e45d8(rdram, ctx, runtime);
        }
    }

    // SWORD_LOAD_GS=<GS.bin>: inyecta la VRAM (4 MB) del savestate del real en el GS del
    // recomp. El teleport de RDRAM (SWORD_LOAD_RDRAM) NO incluye la VRAM ⇒ sin esto el
    // estado del menú queda negro.
    void triageLoadGs(PS2Runtime *runtime)
    {
        const char *gsPath = std::getenv("SWORD_LOAD_GS");
        if (gsPath == nullptr || *gsPath == '\0' || runtime == nullptr)
            return;
        static bool s_gsDone = false;
        if (s_gsDone)
            return;
        s_gsDone = true;
        if (std::FILE *fp = std::fopen(gsPath, "rb"); fp != nullptr)
        {
            static std::vector<uint8_t> s_buf;
            s_buf.assign(0x400000u, 0);
            const size_t n = std::fread(s_buf.data(), 1u, s_buf.size(), fp);
            std::fclose(fp);
            uint8_t *vram = runtime->memory().getGSVRAM();
            if (vram != nullptr && n == 0x400000u)
            {
                std::memcpy(vram, s_buf.data(), 0x400000u);
                RUNTIME_LOG("[load-gs] VRAM 4 MB inyectada desde " << gsPath);
            }
            else
            {
                RUNTIME_LOG("[load-gs] FAIL size=" << n << " path=" << gsPath);
            }
        }
    }

    void triage4848hook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t gp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[28]));
        uint32_t g5ec = 0, g610 = 0, g614 = 0, g638 = 0, g63c = 0;
        const uint32_t addrs[5] = {0x5ECu, 0x610u, 0x614u, 0x638u, 0x63Cu};
        uint32_t *outs[5] = {&g5ec, &g610, &g614, &g638, &g63c};
        for (int i = 0; i < 5; ++i) {
            const uint32_t a = (gp + addrs[i]) & 0x01FFFFFFu;
            if (a + 4 <= 0x02000000u) std::memcpy(outs[i], rdram + a, 4);
        }
        static int s_n = 0;
        static uint32_t s_pa = 0, s_pb = 0;
        const bool chg = (s_n > 0) && (g610 != s_pa || g614 != s_pb);
        if (++s_n % 300 == 1 || chg) {
            RUNTIME_LOG("[triage-4848] n=" << s_n << " 5ec=0x" << std::hex << g5ec << " a610=0x" << g610 << " b614=0x" << g614 << " 638=0x" << g638 << " 63c=0x" << g63c << std::dec << (chg ? " FLIP" : ""));
        }
        s_pa = g610; s_pb = g614;
        if (g_orig4848 != nullptr) {
            g_orig4848(rdram, ctx, runtime);
        }
    }
    // 2026-10-08 TRIAGE: ¿corre el init del motor de DMA?
    void triage6e68e0hook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static int s_n = 0;
        if (++s_n <= 20 || s_n % 200 == 0) {
            RUNTIME_LOG("[motorinit] 0x6e68e0 reset motor n=" << s_n
                        << " ra=0x" << std::hex << static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]))
                        << " a0=0x" << static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) << std::dec);
        }
        if (g_orig6e68e0 != nullptr) {
            g_orig6e68e0(rdram, ctx, runtime);
        }
    }
    void triage6e6934hook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static int s_n = 0;
        if (++s_n <= 20 || s_n % 200 == 0) {
            RUNTIME_LOG("[motorinit] 0x6e6934 instala cb n=" << s_n
                        << " ra=0x" << std::hex << static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]))
                        << " a0=0x" << static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) << std::dec);
        }
        if (g_orig6e6934 != nullptr) {
            g_orig6e6934(rdram, ctx, runtime);
        }
    }
    // Entrada REAL de la funcion que contiene el reset (0x6e68e0 es interior).
    void triage6e6640hook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static int s_n = 0;
        if (++s_n <= 20 || s_n % 200 == 0) {
            RUNTIME_LOG("[motorinit] 0x6e6640 (fn del reset) n=" << s_n
                        << " ra=0x" << std::hex << static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]))
                        << " a0=0x" << static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) << std::dec);
        }
        if (g_orig6e6640 != nullptr) {
            g_orig6e6640(rdram, ctx, runtime);
        }
    }
    // 2026-10-08 TRIAGE: 0x4d1a60 = espera/sync de DMA usada por el spin 0x6f0920.
    // Loguea a0 de entrada y el v0 de salida (el spin repite mientras v0 != 0).
    void triage4d1a60hook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t a0 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
        const uint32_t ra = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        if (g_orig4d1a60 != nullptr) {
            g_orig4d1a60(rdram, ctx, runtime);
        }
        const uint32_t v0 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[2]));
        static uint64_t s_n = 0;
        const bool nonzero = (v0 != 0);
        if (s_n < 30ull || (nonzero && (s_n % 500ull) == 0ull) || (!nonzero && s_n < 30ull)) {
            RUNTIME_LOG("[dmasync] n=" << s_n << " a0=" << a0 << " ret=0x" << std::hex << v0
                        << " ra=0x" << ra << std::dec << (nonzero ? " NONZERO" : " ZERO"));
        }
        ++s_n;
    }
    // 2026-10-08 TRIAGE: 0x6efd28 = funcion de setup que contiene el spin 0x6f0920
    // y el jal 0x6e6640 (init del motor). ¿Corre?
    void triage6efd28hook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static int s_n = 0;
        if (++s_n <= 20 || s_n % 200 == 0) {
            RUNTIME_LOG("[setupfn] 0x6efd28 n=" << s_n
                        << " ra=0x" << std::hex << static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]))
                        << " a0=0x" << static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) << std::dec);
        }
        if (g_orig6efd28 != nullptr) {
            g_orig6efd28(rdram, ctx, runtime);
        }
    }
    std::mutex &registryMutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    std::vector<ps2_game_overrides::Descriptor> &descriptorRegistry()
    {
        static std::vector<ps2_game_overrides::Descriptor> registry;
        return registry;
    }

    bool equalsIgnoreCaseAscii(std::string_view lhs, std::string_view rhs)
    {
        if (lhs.size() != rhs.size())
        {
            return false;
        }

        for (size_t i = 0; i < lhs.size(); ++i)
        {
            const auto l = static_cast<unsigned char>(lhs[i]);
            const auto r = static_cast<unsigned char>(rhs[i]);
            if (std::tolower(l) != std::tolower(r))
            {
                return false;
            }
        }

        return true;
    }

    std::string basenameFromPath(const std::string &path)
    {
        std::error_code ec;
        const std::filesystem::path fsPath(path);
        const std::filesystem::path leaf = fsPath.filename();
        if (leaf.empty())
        {
            return path;
        }
        return leaf.string();
    }

    std::optional<PS2Runtime::RecompiledFunction> resolveHandlerByName(std::string_view handlerName)
    {
        const std::string_view resolvedSyscall = ps2_runtime_calls::resolveSyscallName(handlerName);
        if (!resolvedSyscall.empty())
        {
#define PS2_RESOLVE_SYSCALL(name)                   \
    if (resolvedSyscall == std::string_view{#name}) \
    {                                               \
        return &ps2_syscalls::name;                 \
    }
            PS2_SYSCALL_LIST(PS2_RESOLVE_SYSCALL)
#undef PS2_RESOLVE_SYSCALL
        }

        const std::string_view resolvedStub = ps2_runtime_calls::resolveStubName(handlerName);
        if (!resolvedStub.empty())
        {
#define PS2_RESOLVE_STUB(name)                   \
    if (resolvedStub == std::string_view{#name}) \
    {                                            \
        return &ps2_stubs::name;                 \
    }
            PS2_STUB_LIST(PS2_RESOLVE_STUB)
#undef PS2_RESOLVE_STUB
        }

        return std::nullopt;
    }
}
namespace ps2_game_overrides
{
    AutoRegister::AutoRegister(const Descriptor &descriptor)
    {
        registerDescriptor(descriptor);
    }

    void registerDescriptor(const Descriptor &descriptor)
    {
        if (!descriptor.apply)
        {
            std::cerr << "[game_overrides] ignoring descriptor with null apply callback." << std::endl;
            return;
        }

        std::lock_guard<std::mutex> lock(registryMutex());
        descriptorRegistry().push_back(descriptor);
    }

    bool bindAddressHandler(PS2Runtime &runtime, uint32_t address, std::string_view handlerName)
    {
        const auto resolved = resolveHandlerByName(handlerName);
        if (!resolved.has_value())
        {
            std::cerr << "[game_overrides] unresolved handler '" << handlerName
                      << "' for address 0x" << std::hex << address << std::dec << std::endl;
            return false;
        }

        return runtime.replaceFunction(address, resolved.value());
    }

    void applyMatching(PS2Runtime &runtime,
                       const std::string &elfPath,
                       uint32_t entry,
                       uint32_t fileCrc32,
                       bool fileCrcValid)
    {

        std::vector<Descriptor> descriptors;
        {
            std::lock_guard<std::mutex> lock(registryMutex());
            descriptors = descriptorRegistry();
        }

        if (descriptors.empty())
        {
            return;
        }

        const std::string elfName = basenameFromPath(elfPath);
        size_t appliedCount = 0;
        for (const Descriptor &descriptor : descriptors)
        {
            if (!descriptor.apply)
            {
                continue;
            }

            if (descriptor.elfName && descriptor.elfName[0] != '\0')
            {
                if (!equalsIgnoreCaseAscii(descriptor.elfName, elfName))
                {
                    continue;
                }
            }

            if (descriptor.entry != 0u && descriptor.entry != entry)
            {
                continue;
            }

            if (descriptor.crc32 != 0u)
            {
                if (!fileCrcValid || fileCrc32 != descriptor.crc32)
                {
                    continue;
                }
            }

            const char *name = (descriptor.name && descriptor.name[0] != '\0')
                                   ? descriptor.name
                                   : "unnamed";
            RUNTIME_LOG("[game_overrides] applying '" << name << "'");
            descriptor.apply(runtime);
            ++appliedCount;
        }

        if (appliedCount > 0)
        {
            RUNTIME_LOG("[game_overrides] applied " << appliedCount << " matching override(s).");
        }
    }
}

// TRIAGE correlación 2026-10-05 (external linkage en el namespace que ve
// iop_emulator.cpp): flag para loguear llamadas libsd DURANTE transfers
// 0x30000. Se setea en pre y se limpia en post (ver rama híbrida abajo).
namespace ps2x { namespace iop { namespace detail {
bool g_tyosdTransferActive = false;
} } }

namespace
{
    struct SyscallStubEntry
    {
        uint32_t addr;
        int32_t syscallId;
    };

    // moduleId del driver de sonido real (SDRDRV.IRX) si la precarga funcionó.
    // Mientras sea <= 0 el HLE de sid 0x573 atiende el RPC; si el IRX carga y
    // registra el server, el HLE se desactiva y manda el driver de verdad.
    int g_tyosdRealModuleId = -1;

    // 2026-10-08 (A/B PCSX2): el juego real SÍ llama LoadModule — 11 veces a
    // t=4.48-5.79s, en el orden de `kBootOrder` de abajo, y todos cargan
    // físicos (ret 0/2). Como el recomp saltea ese camino, replicamos orden y
    // semántica: se prefiere SIEMPRE el IRX físico del ISO (código real Sony;
    // id físico < 0x40000000) sobre el provider HLE (id >= 0x40000000).
    void preloadIopBootModules(PS2Runtime *runtime)
    {
        static bool done = false;
        if (done)
        {
            return;
        }
        done = true;

        const auto io = PS2Runtime::getIoPaths();
        std::filesystem::path dir = io.cdRoot / "IOP";
        if (const char *env = std::getenv("SWORD_IOP_DIR"))
        {
            if (*env != '\0')
            {
                dir = env;
            }
        }
        if (!std::filesystem::is_directory(dir))
        {
            RUNTIME_LOG("[iop-preload] no hay directorio IRX en " << dir.string());
            return;
        }

        // Orden REAL de carga del juego (PCSX2, 2026-10-08): SIO2MAN, PADMAN,
        // SIO2D, DBCMAN, MC2_D, CDVDSTM, LIBSD, SDRDRV, LIBSMF2, SDSTR3,
        // SD_CALL.
        static const char *const kBootOrder[] = {
            "SIO2MAN.IRX", "PADMAN.IRX", "SIO2D.IRX", "DBCMAN.IRX", "MC2_D.IRX",
            "CDVDSTM.IRX", "LIBSD.IRX", "SDRDRV.IRX", "LIBSMF2.IRX", "SDSTR3.IRX",
            "SD_CALL.IRX",
        };

        RUNTIME_LOG("[iop-preload] cargando IRX desde " << dir.string());
        for (const char *name : kBootOrder)
        {
            const std::filesystem::path path = dir / name;
            if (!std::filesystem::exists(path))
            {
                RUNTIME_LOG("[iop-preload] falta " << name);
                continue;
            }
            // 1) física desde el ISO primero.
            auto result = runtime->loadIopModule(std::string("cdrom0:/IOP/") + name);
            const char *via = "cdrom0";
            // 2) si cayó en HLE (>= 0x40000000) o falló, probar paths de host.
            if (result.moduleId <= 0 || result.moduleId >= 0x40000000) {
                auto alt = runtime->loadIopModule(path.string());
                if (alt.moduleId > 0 && alt.moduleId < 0x40000000) {
                    result = alt;
                    via = "hostpath";
                } else if (result.moduleId <= 0) {
                    result = alt;
                    via = "hostpath(hle)";
                }
            }
            if (result.moduleId <= 0 || result.moduleId >= 0x40000000) {
                auto alt = runtime->loadIopModule(std::string("host0:IOP/") + name);
                if (alt.moduleId > 0 && alt.moduleId < 0x40000000) {
                    result = alt;
                    via = "host0:IOP";
                } else if (result.moduleId <= 0) {
                    result = alt;
                    via = "host0:IOP(hle)";
                }
            }
            RUNTIME_LOG("[iop-preload] " << name << " moduleId=" << result.moduleId
                                              << " start=" << result.startResult
                                              << " via=" << via);
            if (std::strcmp(name, "SDRDRV.IRX") == 0 && result.moduleId > 0)
            {
                g_tyosdRealModuleId = result.moduleId;
            }
        }
        RUNTIME_LOG("[iop-preload] fin; TYOSD real=" << g_tyosdRealModuleId);
    }

    static const SyscallStubEntry kSwordOfEtheriaSyscallStubs[] = {
        { 0x004d0de8u, 128 },
        { 0x004d2e80u, 0 },
        { 0x004d2e90u, 1 },
        { 0x004d2ea0u, 2 },
        { 0x004d2eb0u, 3 },
        { 0x004d2ec0u, 4 },
        { 0x004d2ed0u, 5 },
        { 0x004d2ee0u, 6 },
        { 0x004d2ef0u, 7 },
        { 0x004d2f00u, 8 },
        { 0x004d2f10u, 9 },
        { 0x004d2f20u, 10 },
        { 0x004d2f30u, 11 },
        { 0x004d2f40u, 12 },
        { 0x004d2f50u, 13 },
        { 0x004d2f60u, 14 },
        { 0x004d2f70u, 15 },
        { 0x004d2f80u, 16 },
        { 0x004d2f90u, 16 },
        { 0x004d2fa0u, 17 },
        { 0x004d2fb0u, 18 },
        { 0x004d2fc0u, 18 },
        { 0x004d2fd0u, 19 },
        { 0x004d2fe0u, 20 },
        { 0x004d2ff0u, 21 },
        { 0x004d3000u, 22 },
        { 0x004d3010u, 23 },
        { 0x004d3020u, 252 },
        { 0x004d3030u, 253 },
        { 0x004d3040u, -26 },
        { 0x004d3050u, -27 },
        { 0x004d3060u, -28 },
        { 0x004d3070u, -29 },
        { 0x004d3080u, -254 },
        { 0x004d3090u, -255 },
        { 0x004d30a0u, 32 },
        { 0x004d30b0u, 33 },
        { 0x004d30c0u, 34 },
        { 0x004d30d0u, 35 },
        { 0x004d30e0u, 36 },
        { 0x004d30f0u, 37 },
        { 0x004d3100u, -38 },
        { 0x004d3110u, 39 },
        { 0x004d3120u, 40 },
        { 0x004d3130u, 41 },
        { 0x004d3140u, -42 },
        { 0x004d3150u, 43 },
        { 0x004d3160u, -44 },
        { 0x004d3170u, 45 },
        { 0x004d3180u, -46 },
        { 0x004d3190u, 47 },
        { 0x004d31a0u, 48 },
        { 0x004d31b0u, -49 },
        { 0x004d31c0u, 50 },
        { 0x004d31d0u, 51 },
        { 0x004d31e0u, -52 },
        { 0x004d31f0u, 53 },
        { 0x004d3200u, -54 },
        { 0x004d3210u, 55 },
        { 0x004d3220u, -56 },
        { 0x004d3230u, 57 },
        { 0x004d3240u, -58 },
        { 0x004d3250u, 59 },
        { 0x004d3260u, 60 },
        { 0x004d3270u, 61 },
        { 0x004d3280u, 62 },
        { 0x004d3290u, 63 },
        { 0x004d32a0u, 64 },
        { 0x004d32b0u, 65 },
        { 0x004d32c0u, 66 },
        { 0x004d32d0u, -67 },
        { 0x004d32e0u, 68 },
        { 0x004d32f0u, 69 },
        { 0x004d3300u, -70 },
        { 0x004d3310u, 71 },
        { 0x004d3320u, -72 },
        { 0x004d3330u, 73 },
        { 0x004d3340u, 74 },
        { 0x004d3350u, 75 },
        { 0x004d3360u, 76 },
        { 0x004d3370u, 77 },
        { 0x004d3380u, 78 },
        { 0x004d3390u, 79 },
        { 0x004d33a0u, 80 },
        { 0x004d33b0u, 81 },
        { 0x004d33c0u, 82 },
        { 0x004d33d0u, -83 },
        { 0x004d33e0u, 84 },
        { 0x004d33f0u, -85 },
        { 0x004d3400u, 86 },
        { 0x004d3410u, 87 },
        { 0x004d3420u, -88 },
        { 0x004d3430u, 89 },
        { 0x004d3440u, -90 },
        { 0x004d3450u, 91 },
        { 0x004d3460u, 92 },
        { 0x004d3470u, -92 },
        { 0x004d3480u, 93 },
        { 0x004d3490u, -93 },
        { 0x004d34a0u, 94 },
        { 0x004d34b0u, -94 },
        { 0x004d34c0u, 95 },
        { 0x004d34d0u, -95 },
        { 0x004d34e0u, 96 },
        { 0x004d34f0u, 97 },
        { 0x004d3500u, 98 },
        { 0x004d3510u, 99 },
        { 0x004d3520u, 100 },
        { 0x004d3530u, 102 },
        { 0x004d3540u, -103 },
        { 0x004d3550u, -104 },
        { 0x004d3560u, -106 },
        { 0x004d3570u, 107 },
        { 0x004d3580u, 108 },
        { 0x004d3590u, 109 },
        { 0x004d35a0u, 110 },
        { 0x004d35b0u, 111 },
        { 0x004d35c0u, 112 },
        { 0x004d35d0u, -112 },
        { 0x004d35e0u, 113 },
        { 0x004d35f0u, -113 },
        { 0x004d3600u, 114 },
        { 0x004d3610u, 115 },
        { 0x004d3620u, 116 },
        { 0x004d3630u, 117 },
        { 0x004d3640u, 118 },
        { 0x004d3650u, -118 },
        { 0x004d3660u, 119 },
        { 0x004d3670u, -119 },
        { 0x004d3680u, 120 },
        { 0x004d3690u, -120 },
        { 0x004d36a0u, 121 },
        { 0x004d36b0u, 122 },
        { 0x004d36c0u, 123 },
        { 0x004d36d0u, 124 },
        { 0x004d36e0u, 125 },
        { 0x004d36f0u, 126 },
        { 0x004d3700u, 127 },
        { 0x004d3710u, 130 },
        { 0x004dcbc8u, 90 },
        { 0x004dcc10u, 91 },
        { 0x004dcc20u, 116 },
        { 0x004dccf8u, 85 },
        { 0x004dcd08u, -85 },
        { 0x004dcd18u, 86 },
        { 0x004dcd58u, -86 },
        { 0x004dcd68u, 87 },
        { 0x004dcd78u, -87 },
        { 0x004dcd88u, 88 },
        { 0x004dcd98u, -88 },
        { 0x004dcda8u, 89 },
        { 0x004dd438u, 90 },
        { 0x004dd4c0u, 131 },
        { 0x004dd608u, 116 },
        { 0x004dd670u, 116 },
        { 0x004dd680u, 90 },
        { 0x004dd6c8u, 91 },
        { 0x004dd7f0u, 90 },
        { 0x004dd830u, 116 },
        { 0x004dda90u, 116 },
        { 0x004ddaa0u, 90 },
        { 0x004ddae8u, 91 },
    };

    static int32_t findSwordOfEtheriaSyscallId(uint32_t addr)
    {
        auto it = std::lower_bound(
            std::begin(kSwordOfEtheriaSyscallStubs),
            std::end(kSwordOfEtheriaSyscallStubs),
            addr,
            [](const SyscallStubEntry &entry, uint32_t target) {
                return entry.addr < target;
            });
        if (it != std::end(kSwordOfEtheriaSyscallStubs) && it->addr == addr)
        {
            return it->syscallId;
        }
        return 0;
    }

    static void swordOfEtheriaSyscallStub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stubPc = ctx->pc;
        const int32_t syscallId = findSwordOfEtheriaSyscallId(stubPc);

        // TRIAGE-sema 2026-10-07: el hilo 2 queda parkeado en WaitSema
        // (0x4d3fa0 -> 0x4d32e0) sobre un semáforo que aparentemente nunca se
        // postea. Trazar TODO op de semaforo (64..70), con el `ra` del que
        // llama, para nombrar quién espera/señala. Log acotado.
        {
            const int32_t sid = (syscallId < 0) ? -syscallId : syscallId;
            if (sid >= 64 && sid <= 70) {
                const uint32_t semaArg = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
                const uint32_t callerRa = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
                // Backtrace guest: los wrappers del juego (p.ej. 0x51ee70) guardan su $ra
                // en el frame; el stub (0x4d32e0) no tiene frame, asi que sp es el del wrapper.
                const uint32_t gsp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));
                uint32_t bt0 = 0u, bt1 = 0u, bt2 = 0u;
                if (rdram != nullptr) {
                    std::memcpy(&bt0, rdram + ((gsp + 0x10u) & 0x1FFFFFFFu), 4);
                    std::memcpy(&bt1, rdram + ((gsp + 0x20u) & 0x1FFFFFFFu), 4);
                    std::memcpy(&bt2, rdram + ((gsp + 0x30u) & 0x1FFFFFFFu), 4);
                }
                static uint64_t s_all = 0, s_focus = 0;
                ++s_all;
                const bool focus = (semaArg == 3u);
                if (focus) ++s_focus;
                if (s_all <= 40ull || (s_all % 5000ull) == 0ull || (focus && s_focus <= 60ull)) {
                    RUNTIME_LOG("[triage-sema] op=" << syscallId << " sema=" << semaArg
                                << " ra=0x" << std::hex << callerRa
                                << " sp=0x" << gsp
                                << " bt=0x" << bt0 << ",0x" << bt1 << ",0x" << bt2 << std::dec
                                << " n=" << s_all);
                }
            }
        }

        // Set $v1 (GPR 3) to syscall ID
        ctx->r[3] = _mm_cvtsi64_si128(static_cast<int64_t>(syscallId));

        // Execute syscall via runtime dispatcher
        runtime->handleSyscall(rdram, ctx, 0);

        if (syscallId == 64) {
            RUNTIME_LOG("[triage-sema] CreateSema -> id="
                        << static_cast<int32_t>(_mm_cvtsi128_si32(ctx->r[2])));
        }

        // MIPS syscall stubs return to $ra (GPR 31) via jr $ra
        ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
    }

    static void swordOfEtheriaInitKernelTableStub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        RUNTIME_LOG("[game_overrides] FUN_004dd508 (kernel table init) bypassed.");
        // Store standard kernel table base to 0x874488
        constexpr uint32_t kTableAddr = 0x80011F80u;
        std::memcpy(rdram + (0x874488u & 0x1FFFFFFFu), &kTableAddr, sizeof(kTableAddr));

        // Return to caller ($ra)
        ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
    }

    static void executeMipsStraightLine(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t startPc)
    {
        (void)runtime;
        auto getGpr32 = [&](uint32_t reg) -> uint32_t {
            return (reg == 0u) ? 0u : static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[reg]));
        };
        auto setGpr32 = [&](uint32_t reg, uint32_t val) {
            if (reg != 0u) {
                ctx->r[reg] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<int32_t>(val)));
            }
        };

        uint32_t pc = startPc;
        while (true)
        {
            const uint32_t raw = *reinterpret_cast<const uint32_t *>(rdram + (pc & 0x01FFFFFFu));
            const uint32_t op = (raw >> 26) & 0x3fu;
            const uint32_t rs = (raw >> 21) & 0x1fu;
            const uint32_t rt = (raw >> 16) & 0x1fu;
            const uint32_t rd = (raw >> 11) & 0x1fu;
            const uint32_t funct = raw & 0x3fu;

            if (op == 0x00u) // SPECIAL
            {
                if (funct == 8u) // jr $ra or jr $rs
                {
                    const uint32_t jumpTarget = getGpr32(rs);
                    // Execute delay slot instruction at pc + 4
                    const uint32_t delayRaw = *reinterpret_cast<const uint32_t *>(rdram + ((pc + 4u) & 0x01FFFFFFu));
                    const uint32_t dop = (delayRaw >> 26) & 0x3fu;
                    const uint32_t drs = (delayRaw >> 21) & 0x1fu;
                    const uint32_t drt = (delayRaw >> 16) & 0x1fu;
                    if (dop == 0x09u) // addiu
                    {
                        const int32_t simm = static_cast<int16_t>(delayRaw & 0xFFFFu);
                        setGpr32(drt, getGpr32(drs) + simm);
                    }
                    else if (dop == 0x2bu) // sw
                    {
                        const int32_t simm = static_cast<int16_t>(delayRaw & 0xFFFFu);
                        const uint32_t addr = getGpr32(drs) + simm;
                        *reinterpret_cast<uint32_t *>(rdram + (addr & 0x01FFFFFFu)) = getGpr32(drt);
                    }
                    else if (dop == 0x39u) // swc1
                    {
                        const int32_t simm = static_cast<int16_t>(delayRaw & 0xFFFFu);
                        const uint32_t addr = getGpr32(drs) + simm;
                        uint32_t val = 0;
                        std::memcpy(&val, &ctx->f[drt], 4);
                        *reinterpret_cast<uint32_t *>(rdram + (addr & 0x01FFFFFFu)) = val;
                    }
                    ctx->pc = jumpTarget;
                    return;
                }
                // funct == 0 is nop, do nothing
            }
            else if (op == 0x09u) // addiu
            {
                const int32_t simm = static_cast<int16_t>(raw & 0xFFFFu);
                setGpr32(rt, getGpr32(rs) + simm);
            }
            else if (op == 0x0du) // ori
            {
                const uint32_t uimm = raw & 0xFFFFu;
                setGpr32(rt, getGpr32(rs) | uimm);
            }
            else if (op == 0x0fu) // lui
            {
                const uint32_t uimm = (raw & 0xFFFFu) << 16u;
                setGpr32(rt, uimm);
            }
            else if (op == 0x23u) // lw
            {
                const int32_t simm = static_cast<int16_t>(raw & 0xFFFFu);
                const uint32_t addr = getGpr32(rs) + simm;
                const uint32_t val = *reinterpret_cast<const uint32_t *>(rdram + (addr & 0x01FFFFFFu));
                setGpr32(rt, val);
            }
            else if (op == 0x2bu) // sw
            {
                const int32_t simm = static_cast<int16_t>(raw & 0xFFFFu);
                const uint32_t addr = getGpr32(rs) + simm;
                *reinterpret_cast<uint32_t *>(rdram + (addr & 0x01FFFFFFu)) = getGpr32(rt);
            }
            else if (op == 0x31u) // lwc1
            {
                const int32_t simm = static_cast<int16_t>(raw & 0xFFFFu);
                const uint32_t addr = getGpr32(rs) + simm;
                std::memcpy(&ctx->f[rt], rdram + (addr & 0x01FFFFFFu), 4);
            }
            else if (op == 0x39u) // swc1
            {
                const int32_t simm = static_cast<int16_t>(raw & 0xFFFFu);
                const uint32_t addr = getGpr32(rs) + simm;
                uint32_t val = 0;
                std::memcpy(&val, &ctx->f[rt], 4);
                *reinterpret_cast<uint32_t *>(rdram + (addr & 0x01FFFFFFu)) = val;
            }
            else if (op == 0x11u) // COP1
            {
                if (rs == 4u) // mtc1
                {
                    const uint32_t val = getGpr32(rt);
                    std::memcpy(&ctx->f[rd], &val, 4);
                }
                else if (rs == 0x10u && funct == 2u) // mul.s
                {
                    const uint32_t fs = rd;
                    const uint32_t ft = rt;
                    const uint32_t fd = (raw >> 6) & 0x1fu;
                    ctx->f[fd] = ctx->f[fs] * ctx->f[ft];
                }
            }
            else
            {
                RUNTIME_LOG("[game_overrides] executeMipsStraightLine unhandled op=0x" << std::hex << op << " at 0x" << pc);
            }

            pc += 4u;
        }
    }

    // 2026-10-09 (Fase 0): probes del init/exit de SIF del juego. FUN_004cf158
    // setea el arg en 0x872098 y bindea 0x80000593; FUN_004d6380 baja 0x873D78.
    static void probeCf158wrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static int s = 0;
        const uint32_t a0 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
        const int n = ++s;
        if (n <= 8) {
            RUNTIME_LOG("[probe-cf158] ENTER#" << n << " a0=0x" << std::hex << a0 << std::dec);
        }
        if (g_origCf158 != nullptr) {
            g_origCf158(rdram, ctx, runtime);
        }
        if (n <= 8) {
            RUNTIME_LOG("[probe-cf158] EXIT#" << n << " ret=0x" << std::hex
                        << static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[2])) << std::dec);
        }
    }

    static void probeD6380wrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static int s = 0;
        if (++s <= 8) {
            RUNTIME_LOG("[probe-d6380] SIF-exit#" << s);
        }
        if (g_origD6380 != nullptr) {
            g_origD6380(rdram, ctx, runtime);
        }
    }

    static void probeCf360wrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static int s = 0;
        const int n = ++s;
        if (n <= 8) {
            RUNTIME_LOG("[probe-cf360] ENTER#" << n);
        }
        if (g_origCf360 != nullptr) {
            g_origCf360(rdram, ctx, runtime);
        }
        if (n <= 8) {
            RUNTIME_LOG("[probe-cf360] EXIT#" << n);
        }
    }

    static void probeCfc60wrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static int s = 0;
        if (++s <= 8) {
            RUNTIME_LOG("[probe-cfc60] ENTER#" << s);
        }
        if (g_origCfc60 != nullptr) {
            g_origCfc60(rdram, ctx, runtime);
        }
    }

    static void applySwordOfEtheriaOverrides(PS2Runtime &runtime)
    {
        // Silence function trace logging unless PS2_FUNCTION_TRACE=1 to avoid
        // synchronous disk writes that drop guest execution from 60 FPS to ~0.3 FPS.
        const char *traceEnv = std::getenv("PS2_FUNCTION_TRACE");
        if (!traceEnv || std::string_view(traceEnv) != "1") {
            ps2_log::log_stream().setstate(std::ios::badbit);
        }

        // Configure IO Paths for Sword of Etheria disc access
        PS2Runtime::IoPaths ioPaths = PS2Runtime::getIoPaths();
        std::error_code ec;
        const std::vector<std::string> isoCandidates = {
            "Sword of Etheria, The (Europe) (En,Fr,De,Es,It).iso",
            "../../Sword of Etheria, The (Europe) (En,Fr,De,Es,It).iso",
            "../Sword of Etheria, The (Europe) (En,Fr,De,Es,It).iso",
            "/home/lubonch/repos/Sword-Of-Etheria-PC-Recomp/Sword of Etheria, The (Europe) (En,Fr,De,Es,It).iso"
        };
        for (const auto &cand : isoCandidates) {
            if (std::filesystem::exists(cand, ec) && !ec) {
                ioPaths.cdImage = std::filesystem::canonical(cand, ec);
                RUNTIME_LOG("[game_overrides] CD image set to: " << ioPaths.cdImage);
                break;
            }
        }

        const std::vector<std::string> elfCandidates = {
            "work/elf",
            "../elf",
            "../../work/elf",
            "/home/lubonch/repos/Sword-Of-Etheria-PC-Recomp/work/elf"
        };
        for (const auto &cand : elfCandidates) {
            if (std::filesystem::exists(cand, ec) && !ec) {
                ioPaths.cdRoot = std::filesystem::canonical(cand, ec);
                RUNTIME_LOG("[game_overrides] CD root set to: " << ioPaths.cdRoot);
                break;
            }
        }
        PS2Runtime::setIoPaths(ioPaths);

        RUNTIME_LOG("[game_overrides] Registering " << std::size(kSwordOfEtheriaSyscallStubs)
                    << " syscall stubs for Sword of Etheria (SLES_537.68)...");
        for (const auto &stub : kSwordOfEtheriaSyscallStubs)
        {
            runtime.replaceFunction(stub.addr, &swordOfEtheriaSyscallStub);
        }

        // Bloqueante #1: CRT0 bootstrap continuation (0x004c008c -> _init_sys 0x004dd618)
        static auto entryContinuation4c008c = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            RUNTIME_LOG("[game_overrides] CRT0 bootstrap continuation at 0x4c008c executing...");

            // 1. Clear FPU registers ($f0 - $f31)
            for (int i = 0; i < 32; ++i) {
                ctx->f[i] = 0.0f;
            }

            // 2. Clear BSS (0x00a27c80 to 0x00a93700)
            const uint32_t bssStart = 0x00a27c80u;
            const uint32_t bssEnd   = 0x00a93700u;
            if (bssEnd > bssStart && (bssEnd & 0x01FFFFFFu) <= 0x02000000u) {
                std::memset(rdram + (bssStart & 0x01FFFFFFu), 0, bssEnd - bssStart);
            }

            // 3. Setup $gp and $sp
            const uint32_t gpVal = 0x00a28070u;
            const uint32_t spVal = 0x01fee000u;
            ctx->r[28] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<int32_t>(gpVal)));
            ctx->r[29] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<int32_t>(spVal)));

            // 4. SetupHeap(base = 0x00a93700, size = -1) via syscall 0x3d
            ctx->r[4] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<int32_t>(bssEnd)));
            ctx->r[5] = _mm_cvtsi64_si128(-1);
            ctx->r[3] = _mm_cvtsi64_si128(0x3d);
            runtime->handleSyscall(rdram, ctx, 0);

            // 5. Call the CRT init with return address $ra = 0x004c01f0.
            //    El crt0 REAL llama 0x4DDA18 (jal @0x4c01e8); el runtime usa 0x4DD618.
            //    SWORD_CRT0_INIT=da18 usa el target real para A/B (env, sin recompilar).
            static const uint32_t s_crt0Init = []() -> uint32_t {
                const char *e = std::getenv("SWORD_CRT0_INIT");
                if (e && (e[0] == 'd' || e[0] == 'D') && e[1] == 'a') return 0x004dda18u;
                if (e && (e[0] == 'd' || e[0] == 'D') && e[1] == 'd') return 0x004dd958u;
                return 0x004dd618u;
            }();
            ctx->r[31] = _mm_cvtsi64_si128(0x004c01f0u);
            ctx->pc = s_crt0Init;
            RUNTIME_LOG("[game_overrides] CRT0 init target = 0x" << std::hex << s_crt0Init << std::dec);
            (void)runtime->dispatchGuestBranch(rdram, ctx, s_crt0Init, 0x004c01e8u, 0x004c01f0u, PS2Runtime::GuestBranchKind::DirectCall, "JAL");
        };
        runtime.replaceFunction(0x004c008cu, entryContinuation4c008c);

        // 0x004d6040: CONTINUACION de FUN_004d6008 (Ghidra la corto a 0x4D6040; el recomp
        // no la genero). La ejecuta el init REAL del crt0 (0x4DDA18 -> ... -> 0x4D6008
        // fallthrough). Sin esto, el scheduler ve pc=0x4D6040 sin funcion -> pc=0 (idle).
        {
            auto rd32 = [](const uint8_t *r, uint32_t a) -> uint32_t { uint32_t v = 0; if (a + 4u <= PS2_RAM_SIZE) std::memcpy(&v, r + a, 4); return v; };
            auto gpr = [](R5900Context *c, int i) -> uint32_t { return static_cast<uint32_t>(_mm_cvtsi128_si32(c->r[i])); };
            auto setg = [](R5900Context *c, int i, uint32_t v) { c->r[i] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<int32_t>(v))); };
            // 0x4D6078: lds1/lds0/epilogue
            auto epilogue = [rd32, gpr, setg](uint8_t *r, R5900Context *c) {
                const uint32_t sp = gpr(c, 29);
                setg(c, 31, rd32(r, sp + 0x30u));
                setg(c, 18, rd32(r, sp + 0x20u));
                setg(c, 16, rd32(r, sp + 0x10u));
                setg(c, 29, sp + 0xC0u);
                c->pc = gpr(c, 31);
            };
            static auto cont4d6040 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
                auto gpr = [](R5900Context *c, int i) -> uint32_t { return static_cast<uint32_t>(_mm_cvtsi128_si32(c->r[i])); };
                auto setg = [](R5900Context *c, int i, uint32_t v) { c->r[i] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<int32_t>(v))); };
                const uint32_t sp = gpr(ctx, 29);
                setg(ctx, 18, gpr(ctx, 2));       // s1 = v0 (retorno de func_4DD360)
                setg(ctx, 4, 0x004D5E20u);        // a0
                setg(ctx, 5, sp);                 // a1
                setg(ctx, 6, 0xFFFFFFFFu);        // a2
                setg(ctx, 7, gpr(ctx, 16));       // a3 = s0
                setg(ctx, 8, sp + 0x88u);         // t0
                ctx->r[31] = _mm_cvtsi64_si128(0x004D6064u);
                ctx->pc = 0x004D5088u;
                (void)runtime->dispatchGuestBranch(rdram, ctx, 0x004D5088u, 0x004D6040u, 0x004D6064u, PS2Runtime::GuestBranchKind::DirectCall, "JAL");
            };
            static auto cont4d6064 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
                auto gpr = [](R5900Context *c, int i) -> uint32_t { return static_cast<uint32_t>(_mm_cvtsi128_si32(c->r[i])); };
                auto setg = [](R5900Context *c, int i, uint32_t v) { c->r[i] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<int32_t>(v))); };
                if (gpr(ctx, 18) != 0u) { // s1 != 0
                    ctx->r[31] = _mm_cvtsi64_si128(0x004D6074u);
                    ctx->pc = 0x004DD3B0u;
                    (void)runtime->dispatchGuestBranch(rdram, ctx, 0x004DD3B0u, 0x004D606Cu, 0x004D6074u, PS2Runtime::GuestBranchKind::DirectCall, "JAL");
                    return;
                }
                const uint32_t sp = gpr(ctx, 29);
                uint32_t v = 0;
                if (sp + 0x34u <= PS2_RAM_SIZE) { std::memcpy(&v, rdram + sp + 0x30u, 4); setg(ctx, 31, v); }
                if (sp + 0x24u <= PS2_RAM_SIZE) { std::memcpy(&v, rdram + sp + 0x20u, 4); setg(ctx, 18, v); }
                if (sp + 0x14u <= PS2_RAM_SIZE) { std::memcpy(&v, rdram + sp + 0x10u, 4); setg(ctx, 16, v); }
                setg(ctx, 29, sp + 0xC0u);
                ctx->pc = gpr(ctx, 31);
            };
            static auto cont4d6074 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
                (void)runtime;
                auto gpr = [](R5900Context *c, int i) -> uint32_t { return static_cast<uint32_t>(_mm_cvtsi128_si32(c->r[i])); };
                auto setg = [](R5900Context *c, int i, uint32_t v) { c->r[i] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<int32_t>(v))); };
                const uint32_t sp = gpr(ctx, 29);
                uint32_t v = 0;
                if (sp + 0x34u <= PS2_RAM_SIZE) std::memcpy(&v, rdram + sp + 0x30u, 4); setg(ctx, 31, v);
                if (sp + 0x24u <= PS2_RAM_SIZE) std::memcpy(&v, rdram + sp + 0x20u, 4); setg(ctx, 18, v);
                if (sp + 0x14u <= PS2_RAM_SIZE) std::memcpy(&v, rdram + sp + 0x10u, 4); setg(ctx, 16, v);
                setg(ctx, 29, sp + 0xC0u);
                ctx->pc = gpr(ctx, 31);
            };
            (void)epilogue;
            runtime.replaceFunction(0x004d6040u, cont4d6040);
            runtime.replaceFunction(0x004d6064u, cont4d6064);
            runtime.replaceFunction(0x004d6074u, cont4d6074);
        }

        // 0x00627f50: encolador de eventos (buffer 0xA44100 + (idx<<4); contador en gp+0x44).
        // CAUSA RAIZ encontrada: el stub triage627f50 de mas abajo (linea ~4400) abandonaba
        // la funcion a mitad (pc=0x627F54) y pisaba este override. Ya esta DESACTIVADO.
        // Con eso corre la funcion REAL recompilada. Este override queda como A/B, default OFF.
        // SWORD_FIX_627F50=1 para activarlo.
        static const bool s_fix627f50 = []() {
            const char *e = std::getenv("SWORD_FIX_627F50");
            return (e != nullptr && e[0] == '1');
        }();
        if (s_fix627f50) {
            static auto fix627f50 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
                (void)runtime;
                auto gpr = [](R5900Context *c, int i) -> uint32_t { return static_cast<uint32_t>(_mm_cvtsi128_si32(c->r[i])); };
                auto setg = [](R5900Context *c, int i, uint32_t v) { c->r[i] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<int32_t>(v))); };
                auto rd32 = [](const uint8_t *r, uint32_t a) -> uint32_t { uint32_t v = 0; if (a + 4u <= PS2_RAM_SIZE) std::memcpy(&v, r + a, 4); return v; };
                auto wr32 = [](uint8_t *r, uint32_t a, uint32_t v) { if (a + 4u <= PS2_RAM_SIZE) std::memcpy(r + a, &v, 4); };
                const uint32_t gp = gpr(ctx, 28);
                const uint32_t count = rd32(rdram, gp + 0x44u);
                if (count < 0x41u) {
                    const uint32_t base = 0x00a44100u + (count << 4);
                    wr32(rdram, base + 0x0u, gpr(ctx, 4));
                    wr32(rdram, base + 0x4u, gpr(ctx, 5));
                    wr32(rdram, base + 0x8u, gpr(ctx, 6));
                    wr32(rdram, base + 0xCu, gpr(ctx, 7));
                    wr32(rdram, gp + 0x44u, count + 1u);
                    setg(ctx, 2, 0u);
                } else {
                    setg(ctx, 2, 0xFFFFFFFFu);
                }
                ctx->pc = gpr(ctx, 31);
            };
            runtime.replaceFunction(0x00627f50u, fix627f50);
        }

        // 0x004de480: CONTINUACION de entry_004de478 (Ghidra la corto; `ei` + epilogo).
        // El juego loopea ahi (pc faltante -> pc=0). SWORD_FIX_4DE480=0 para A/B.
        static const bool s_fix4de480 = []() {
            const char *e = std::getenv("SWORD_FIX_4DE480");
            return !(e != nullptr && *e == '0');
        }();
        if (s_fix4de480)
        {
            static auto cont4de480 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
                (void)runtime;
                auto ld = [rdram](uint32_t a) -> uint32_t { uint32_t v = 0; if (a + 4u <= PS2_RAM_SIZE) std::memcpy(&v, rdram + a, 4); return v; };
                auto gpr = [ctx](int i) -> uint32_t { return static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[i])); };
                auto setg = [ctx](int i, uint32_t v) { ctx->r[i] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<int32_t>(v))); };
                ctx->cop0_status |= 0x10000u; // ei
                const uint32_t sp = gpr(29);
                setg(31, ld(sp + 0x80u));
                setg(2, 0u);
                setg(23, ld(sp + 0x70u)); setg(22, ld(sp + 0x60u));
                setg(21, ld(sp + 0x50u)); setg(20, ld(sp + 0x40u));
                setg(19, ld(sp + 0x30u)); setg(18, ld(sp + 0x20u));
                setg(17, ld(sp + 0x10u)); setg(16, ld(sp + 0x00u));
                setg(29, sp + 0x90u);
                ctx->pc = gpr(31);
            };
            runtime.replaceFunction(0x004de480u, cont4de480);
        }

        // 0x004dd404: continuation of FUN_004dd3c8 (creates second sema and returns)
        static auto continuation4dd404 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            // 1. Store return value of first call ($v0) to 0x008744a0
            const uint32_t firstRet = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[2]));
            std::memcpy(rdram + (0x008744a0u & 0x01FFFFFFu), &firstRet, 4);

            // 2. Setup $a0 = $sp + 0x20 for second call to 0x4d32a0 (CreateSema)
            const uint32_t sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));
            ctx->r[4] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<int32_t>(sp + 0x20u)));
            ctx->r[31] = _mm_cvtsi64_si128(0x004dd414u);
            ctx->pc = 0x004d32a0u;
            if (!runtime->dispatchGuestBranch(rdram, ctx, 0x004d32a0u, 0x004dd40cu, 0x004dd414u, PS2Runtime::GuestBranchKind::DirectCall, "JAL")) {
                return;
            }

            // 3. Store return value of second call ($v0) to 0x008744a4
            const uint32_t secondRet = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[2]));
            std::memcpy(rdram + (0x008744a4u & 0x01FFFFFFu), &secondRet, 4);

            // 4. Restore $ra from 0x40($sp)
            uint64_t raVal = 0;
            std::memcpy(&raVal, rdram + ((sp + 0x40u) & 0x01FFFFFFu), 8);
            ctx->r[31] = _mm_cvtsi64_si128(raVal);

            // 5. Restore $sp (addiu $sp, $sp, 0x50) and return (jr $ra)
            ctx->r[29] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<int32_t>(sp + 0x50u)));
            ctx->pc = static_cast<uint32_t>(raVal);
        };
        runtime.replaceFunction(0x004dd404u, continuation4dd404);

        // Bloqueante #3: bypass kernel table search loop
        runtime.replaceFunction(0x004dd508u, &swordOfEtheriaInitKernelTableStub);

        // Bloqueante #4: return stub for FUN_004ddaf8 (0x004ddbc4 -> jr $ra; addiu $sp, $sp, 0x40)
        static auto returnStub4ddbc4 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            RUNTIME_LOG("[game_overrides] FUN_004ddaf8 return stub at 0x4ddbc4 executed.");
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x40);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004ddbc4u, returnStub4ddbc4);

        // FUN_004c3a90 (memset) return stub: 0x004c3b48 -> jr $ra; daddu $v0, $a0, $zero
        static auto returnStub4c3b48 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            RUNTIME_LOG("[game_overrides] FUN_004c3a90 return stub at 0x4c3b48 executed.");
            ctx->r[2] = ctx->r[4]; // daddu $v0, $a0, $zero
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004c3b48u, returnStub4c3b48);

        // FUN_004c3b50 (free) return stub: 0x004c3bc4 -> jr $ra; addiu $sp, $sp, 0x20
        static auto returnStub4c3bc4 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x20);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004c3bc4u, returnStub4c3bc4);

        // FUN_004c3bd0 return stub: 0x004c3c14 -> jr $ra; addiu $sp, $sp, 0x10
        static auto returnStub4c3c14 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x10);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004c3c14u, returnStub4c3c14);

        // FUN_005ea650 return stub: 0x005ea75c -> jr $ra; addiu $sp, $sp, 0x10
        static auto returnStub5ea75c = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x10);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x005ea75cu, returnStub5ea75c);

        // FUN_004ddc70 return stub: 0x004dddbc -> jr $ra; addiu $sp, $sp, 0x60
        static auto returnStub4dddbc = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            RUNTIME_LOG("[game_overrides] FUN_004ddc70 return stub at 0x4dddbc executed.");
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x60);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004dddbcu, returnStub4dddbc);

        // FUN_004ddeb0 bypass: Timer 2 hardware init
        static auto bypassStub4ddeb0 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            RUNTIME_LOG("[game_overrides] FUN_004ddeb0 (timer 2 init) bypassed.");
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004ddeb0u, bypassStub4ddeb0);

        // FUN_004d4078 return stub: 0x004d415c -> jr $ra; addiu $sp, $sp, 0x90
        static auto returnStub4d415c = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            RUNTIME_LOG("[game_overrides] FUN_004d4078 return stub at 0x4d415c executed.");
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x90);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d415cu, returnStub4d415c);

        // FUN_004dd740 bypass: SDK thread priority setup (and its truncated helper FUN_004dd6d8)
        static auto bypassStub4dd740 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            RUNTIME_LOG("[game_overrides] FUN_004dd740 (thread priority setup) bypassed.");
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004dd740u, bypassStub4dd740);

        // FUN_004dcc30 bypass: SDK stdio descriptor init
        static auto bypassStub4dcc30 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            RUNTIME_LOG("[game_overrides] FUN_004dcc30 (stdio init) bypassed.");
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004dcc30u, bypassStub4dcc30);

        // FUN_004def40: Memory pool free-list initialization (omitted by Ghidra)
        static auto initPool4def40 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            RUNTIME_LOG("[game_overrides] FUN_004def40 (memory pool init) executed.");
            const uint32_t poolHead = 0x00a31100u;
            std::memcpy(rdram + (0x00a31500u & 0x1FFFFFFFu), &poolHead, sizeof(poolHead));

            uint32_t v1 = poolHead + 1024u; // 0x00a31500
            uint32_t v0 = poolHead + 1008u; // 0x00a314f0
            for (int i = 63; i >= 0; --i)
            {
                std::memcpy(rdram + (v0 & 0x1FFFFFFFu), &v1, sizeof(v1));
                v0 -= 16u;
                v1 -= 16u;
            }
            const uint32_t zero = 0u;
            std::memcpy(rdram + ((poolHead + 1008u) & 0x1FFFFFFFu), &zero, sizeof(zero));

            ctx->r[2] = _mm_setzero_si128();
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004def40u, initPool4def40);

        // 0x004ddc40: SetTimer2Count(value in $a0) -> writes to 0x10001000 and returns to $ra
        static auto setTimer2Count = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram;
            runtime->Store32(rdram, ctx, 0x10001000u, static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])));
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004ddc40u, setTimer2Count);

        // 0x004ddc50: SetTimer2Mode(value in $a0) -> writes to 0x10001010 and returns to $ra
        static auto setTimer2Mode = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram;
            runtime->Store32(rdram, ctx, 0x10001010u, static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])));
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004ddc50u, setTimer2Mode);

        // 0x004ddc60: SetTimer2Compare(value in $a0) -> writes to 0x10001020 and returns to $ra
        static auto setTimer2Compare = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram;
            runtime->Store32(rdram, ctx, 0x10001020u, static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])));
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004ddc60u, setTimer2Compare);

        // 0x004dd360: DIntr() -> disables interrupts, returns previous EDI status in $v0, returns to $ra
        static auto dintrStub4dd360 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            const uint32_t oldStatus = ctx->cop0_status;
            const uint32_t wasEnabled = (oldStatus >> 16u) & 1u;
            ctx->cop0_status &= ~0x10000u;
            ctx->r[2] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<int32_t>(wasEnabled)));
            ctx->r[3] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<int32_t>(oldStatus & 0x10000u)));
            ctx->r[4] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<int32_t>(wasEnabled)));
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004dd360u, dintrStub4dd360);

        // 0x004dd378: DI() -> disables interrupts, $v0 = $a0, returns to $ra
        static auto diStub4dd378 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->cop0_status &= ~0x10000u;
            ctx->r[2] = ctx->r[4];
            ctx->r[3] = _mm_setzero_si128();
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004dd378u, diStub4dd378);

        // 0x004dd3a8: DisableInterrupts return path -> v0 = a0; jr $ra
        static auto returnStub4dd3a8 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->r[2] = ctx->r[4];
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004dd3a8u, returnStub4dd3a8);

        // 0x004dd3b0: EIntr() -> enables interrupts, returns previous EDI status in $v0, returns to $ra
        static auto eintrStub4dd3b0 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            const uint32_t oldStatus = ctx->cop0_status;
            const uint32_t wasEnabled = (oldStatus >> 16u) & 1u;
            ctx->cop0_status |= 0x10000u;
            ctx->r[2] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<int32_t>(wasEnabled)));
            ctx->r[3] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<int32_t>(0x10000u)));
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004dd3b0u, eintrStub4dd3b0);

        // 0x004dd3bc: EnableInterrupts return path -> ei; jr $ra; nop
        static auto returnStub4dd3bc = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->cop0_status |= 0x10000u;
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004dd3bcu, returnStub4dd3bc);

        // 0x004d3e48: FUN_004d3de8 return stub -> jr $ra; addiu $sp, $sp, 0x30
        static auto returnStub4d3e48 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            RUNTIME_LOG("[game_overrides] FUN_004d3de8 return stub at 0x4d3e48 executed.");
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x30);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d3e48u, returnStub4d3e48);

        // 0x004d3d70: iCanDispatch / QueryIntrContext -> returns (cop0_status ^ 1) & 1, returns to $ra
        static auto queryIntrContext4d3d70 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            const uint32_t v0 = (ctx->cop0_status ^ 1u) & 1u;
            ctx->r[2] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<int32_t>(v0)));
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d3d70u, queryIntrContext4d3d70);

        // 0x004d4550: FUN_004d43e0 return stub -> jr $ra; addiu $sp, $sp, 0x70
        static auto returnStub4d4550 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x70);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d4550u, returnStub4d4550);

        // 0x004c01f0: CRT startup continuation after _init_sys (0x4dd618)
        static auto entryContinuation4c01f0 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            RUNTIME_LOG("[game_overrides] CRT0 startup continuation at 0x4c01f0 executing...");

            // Pre-initialize rom0:ROMVER buffer at 0x0096DC38 with PAL Europe retail console version string
            const uint32_t romverPhys = 0x0096DC38u & 0x01FFFFFFu;
            if (romverPhys + 16 <= 0x02000000u) {
                std::memcpy(rdram + romverPhys, "0190EC20040510\n", 16);
            }

            // 0x4c01f0: jal func_4D3520 (CRT global initializers)
            ctx->pc = 0x4C01F0u;
            ctx->r[31] = _mm_cvtsi64_si128(0x4C01F8u);
            ctx->r[4] = _mm_setzero_si128(); // move $a0, $zero
            ctx->pc = 0x4D3520u;
            // 2026-10-07c (checkpoint-due): dispatchGuestBranch devuelve false
            // TAMBIÉN cuando el scheduler pide yield (checkpoint due) sin haber
            // corrido el target. Abandonar acá mata el boot en silencio (el
            // thread queda en pc=target y cae a función inexistente). 4D3520 es
            // solo FlushCache (no-op en host): continuar en fallthrough.
            // 2026-10-07e: mejor que emular: correr el target INLINE con
            // lookupFunction (salta el gate del checkpoint; el boot corre una
            // sola vez). Si ni hay función → fallthrough como antes.
            if (!runtime->dispatchGuestBranch(rdram, ctx, 0x4D3520u, 0x4C01F0u, 0x4C01F8u, PS2Runtime::GuestBranchKind::DirectCall, "JAL")) {
                if (runtime->hasFunction(0x4D3520u)) {
                    RUNTIME_LOG("[game_overrides] CRT0: 4D3520 inline (checkpoint-due)");
                    ctx->r[31] = _mm_cvtsi64_si128(0x4C01F8u);
                    runtime->lookupFunction(0x4D3520u)(rdram, ctx, runtime);
                    ctx->pc = 0x4C01F8u;
                } else {
                    RUNTIME_LOG("[game_overrides] CRT0: 4D3520 dispatch deferred, continuing at 0x4c01f8");
                    ctx->pc = 0x4C01F8u;
                }
            }

            // 0x4c01f8: ei
            ctx->pc = 0x4c01f8u;
            ctx->cop0_status |= 0x10000;

            // 0x4c01fc - 0x4c0204: read argc from 0xa31d00
            const uint32_t argBase = 0x00A31D00u;
            uint32_t argc = 0;
            std::memcpy(&argc, rdram + (argBase & 0x1FFFFFFFu), sizeof(argc));
            ctx->r[4] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<int32_t>(argc)));
            ctx->r[5] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<int32_t>(argBase + 4u)));

            // 0x4c0208: jal func_51FEA0 (argument parsing)
            ctx->pc = 0x4C0208u;
            ctx->r[31] = _mm_cvtsi64_si128(0x4C0210u);
            ctx->pc = 0x51FEA0u;
            // 2026-10-07c: igual que arriba.
            // 2026-10-07f: NO correr inline (51FEA0 es largo y sus yields
            // interiores (eeCheckpointDue) abortarían el init a medias). En
            // defer, pc=target y ra=fallthrough YA quedaron puestos: basta
            // retornar y el scheduler lo corre natural (con sus yields).
            if (!runtime->dispatchGuestBranch(rdram, ctx, 0x51FEA0u, 0x4C0208u, 0x4C0210u, PS2Runtime::GuestBranchKind::DirectCall, "JAL")) {
                RUNTIME_LOG("[game_overrides] CRT0: 51FEA0 deferred to scheduler");
                return;
            }

            // 0x4c0210: j func_4C1900 (main of Sword of Etheria)
            ctx->pc = 0x4C0210u;
            ctx->r[4] = ctx->r[2]; // move $a0, $v0
            ctx->pc = 0x4C1900u;
            RUNTIME_LOG("[game_overrides] JUMPING TO MAIN (0x4c1900)!");
            (void)runtime->dispatchGuestBranch(rdram, ctx, 0x4C1900u, 0x4C0210u, 0x4C0218u, PS2Runtime::GuestBranchKind::DirectJump, "J");
        };
        runtime.replaceFunction(0x004c01f0u, entryContinuation4c01f0);

        // En una PS2 real el CRT0 carga los IRX de IOPRP300 antes de saltar al
        // juego. Este recomp arranca con el IOP vacío (loadedModules=[] en cada
        // trace de RPC), así que no hay servidores de sonido, pad ni CD. El
        // juego nunca llama LoadModule, por eso los precargamos nosotros en el
        // primer sceSifInitRpc (que corre después del resetIop() de run()).
        // 0x004d6860: sceSifInitRpc(mode in $a0) -> Sony SDK SIF RPC initialization
        static auto sifInitRpcStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            preloadIopBootModules(runtime);
            static bool loggedInitOnce = false;
            if (!loggedInitOnce) {
                RUNTIME_LOG("[game_overrides] sceSifInitRpc (0x4d6860) executed.");
                loggedInitOnce = true;
            }
            const uint32_t one = 1;
            std::memcpy(rdram + (0x00873d80u & 0x1FFFFFFFu), &one, sizeof(one));

            // Initialize RPC packet container at 0x00a2c1c0
            const uint32_t s0 = 0x00a2c1c0u;
            const uint32_t zero = 0;
            const uint32_t thirtyTwo = 32;
            const uint32_t tableBase = 0x20a2a9c0u;
            const uint32_t a2 = 0x20a2b1c0u;
            const uint32_t a3 = 0x20a2b9c0u;

            std::memcpy(rdram + (s0 & 0x1FFFFFFFu), &one, sizeof(one));
            std::memcpy(rdram + ((s0 + 4) & 0x1FFFFFFFu), &tableBase, sizeof(tableBase));
            std::memcpy(rdram + ((s0 + 8) & 0x1FFFFFFFu), &thirtyTwo, sizeof(thirtyTwo));
            std::memcpy(rdram + ((s0 + 12) & 0x1FFFFFFFu), &zero, sizeof(zero));
            std::memcpy(rdram + ((s0 + 16) & 0x1FFFFFFFu), &zero, sizeof(zero));
            std::memcpy(rdram + ((s0 + 20) & 0x1FFFFFFFu), &a2, sizeof(a2));
            std::memcpy(rdram + ((s0 + 24) & 0x1FFFFFFFu), &thirtyTwo, sizeof(thirtyTwo));
            std::memcpy(rdram + ((s0 + 28) & 0x1FFFFFFFu), &a3, sizeof(a3));
            std::memcpy(rdram + ((s0 + 32) & 0x1FFFFFFFu), &thirtyTwo, sizeof(thirtyTwo));
            std::memcpy(rdram + ((s0 + 36) & 0x1FFFFFFFu), &zero, sizeof(zero));

            std::memset(rdram + (0x00a2a9c0u & 0x1FFFFFFFu), 0, 32 * 64);
            std::memset(rdram + (0x00a2b1c0u & 0x1FFFFFFFu), 0, 32 * 64);
            std::memset(rdram + (0x00a2b9c0u & 0x1FFFFFFFu), 0, 32 * 64);

            ps2_stubs::sceSifInitRpc(rdram, ctx, runtime);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d6860u, sifInitRpcStub);

        // 0x004d73e8: sceSifCheckStatRpc
        static auto checkStatRpcStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->r[2] = _mm_setzero_si128(); // return 0 (completed / ready)
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d73e8u, checkStatRpcStub);

        // 0x004d7414: sceSifCheckStatRpc return 0 path (jr $ra; move $v0, $zero)
        static auto returnZero4d7414 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->r[2] = _mm_setzero_si128();
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d7414u, returnZero4d7414);

        // 0x004d741c: sceSifCheckStatRpc return 1 path (jr $ra; li $v0, 1)
        static auto returnOne4d741c = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->r[2] = _mm_cvtsi32_si128(1);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d741cu, returnOne4d741c);

        // 0x004d7428: sceSifSetRpcQueue(qd in $a0, thread_id in $a1)
        static auto sifSetRpcQueueStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            RUNTIME_LOG("[game_overrides] sceSifSetRpcQueue (0x4d7428) executed.");
            const uint32_t qd = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            const uint32_t threadId = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
            if (qd != 0 && (qd + 24) <= 0x02000000) {
                const uint32_t zero = 0;
                std::memcpy(rdram + (qd & 0x1FFFFFFFu), &threadId, 4); // thread_id
                std::memcpy(rdram + ((qd + 4) & 0x1FFFFFFFu), &zero, 4); // active
                std::memcpy(rdram + ((qd + 8) & 0x1FFFFFFFu), &zero, 4); // link
                std::memcpy(rdram + ((qd + 12) & 0x1FFFFFFFu), &zero, 4); // start
                std::memcpy(rdram + ((qd + 16) & 0x1FFFFFFFu), &zero, 4); // end
                std::memcpy(rdram + ((qd + 20) & 0x1FFFFFFFu), &zero, 4); // next
            }
            ps2_stubs::sceSifSetRpcQueue(rdram, ctx, runtime);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d7428u, sifSetRpcQueueStub);

        // 0x004d74c0: sceSifRegisterRpc
        static auto sifRegisterRpcStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            RUNTIME_LOG("[game_overrides] sceSifRegisterRpc (0x4d74c0) executed.");
            ps2_stubs::sceSifRegisterRpc(rdram, ctx, runtime);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d74c0u, sifRegisterRpcStub);

        // 0x004d7590: sceSifRemoveRpc
        static auto sifRemoveRpcStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            RUNTIME_LOG("[game_overrides] sceSifRemoveRpc (0x4d7590) executed.");
            ps2_stubs::sceSifRemoveRpc(rdram, ctx, runtime);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d7590u, sifRemoveRpcStub);

        // 0x004d7628: sceSifRemoveRpcQueue
        static auto sifRemoveRpcQueueStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            RUNTIME_LOG("[game_overrides] sceSifRemoveRpcQueue (0x4d7628) executed.");
            ps2_stubs::sceSifRemoveRpcQueue(rdram, ctx, runtime);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d7628u, sifRemoveRpcQueueStub);

        // 0x004d76b8: sceSifGetNextRequest
        static auto sifGetNextRequestStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            ps2_stubs::sceSifGetNextRequest(rdram, ctx, runtime);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d76b8u, sifGetNextRequestStub);

        // 0x004cf14c: FUN_004cf0e8 return stub -> jr $ra; addiu $sp, $sp, 0x20
        static auto returnStub4cf14c = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x20);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004cf14cu, returnStub4cf14c);

        // 0x004d6a28: _sif_alloc_packet(container in $a0) -> returns allocated packet ptr in $v0
        static auto sifAllocPacketStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t container = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            uint32_t allocatedEntry = 0;
            if (container != 0 && (container + 12) <= 0x02000000) {
                uint32_t maxCount = 0, tableBase = 0;
                std::memcpy(&maxCount, rdram + ((container + 8) & 0x1FFFFFFFu), sizeof(maxCount));
                std::memcpy(&tableBase, rdram + ((container + 4) & 0x1FFFFFFFu), sizeof(tableBase));

                for (uint32_t i = 0; i < maxCount; ++i) {
                    const uint32_t entry = tableBase + i * 64u;
                    const uint32_t physEntry = entry & 0x1FFFFFFFu;
                    if ((physEntry + 64u) > 0x02000000) break;

                    uint32_t flag = 0;
                    std::memcpy(&flag, rdram + (physEntry + 16), sizeof(flag));
                    if ((flag & 1) == 0) {
                        const uint32_t newFlag = (i << 16) | 5u;
                        std::memcpy(rdram + (physEntry + 16), &newFlag, sizeof(newFlag));

                        uint32_t seq = 0;
                        std::memcpy(&seq, rdram + (container & 0x1FFFFFFFu), sizeof(seq));
                        uint32_t v1 = seq + 1;
                        if (v1 == 1) {
                            std::memcpy(rdram + (container & 0x1FFFFFFFu), &v1, sizeof(v1));
                        } else {
                            const uint32_t newSeq = seq + 2;
                            v1 = 1;
                            std::memcpy(rdram + (container & 0x1FFFFFFFu), &newSeq, sizeof(newSeq));
                        }

                        std::memcpy(rdram + (physEntry + 20), &entry, sizeof(entry));
                        std::memcpy(rdram + (physEntry + 24), &v1, sizeof(v1));
                        allocatedEntry = entry;
                        break;
                    }
                }
            }
            ctx->r[2] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<int32_t>(allocatedEntry)));
            ctx->cop0_status |= 0x10000; // EnableInterrupts
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d6a28u, sifAllocPacketStub);

        // Fallback for 0x004d6a40 in case FUN_004d6a28 was entered
        static auto sifAllocPacketFallback4d6a40 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t container = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[17])); // $s1
            uint32_t allocatedEntry = 0;
            if (container != 0 && (container + 12) <= 0x02000000) {
                uint32_t maxCount = 0, tableBase = 0;
                std::memcpy(&maxCount, rdram + ((container + 8) & 0x1FFFFFFFu), sizeof(maxCount));
                std::memcpy(&tableBase, rdram + ((container + 4) & 0x1FFFFFFFu), sizeof(tableBase));

                for (uint32_t i = 0; i < maxCount; ++i) {
                    const uint32_t entry = tableBase + i * 64u;
                    const uint32_t physEntry = entry & 0x1FFFFFFFu;
                    if ((physEntry + 64u) > 0x02000000) break;

                    uint32_t flag = 0;
                    std::memcpy(&flag, rdram + (physEntry + 16), sizeof(flag));
                    if ((flag & 1) == 0) {
                        const uint32_t newFlag = (i << 16) | 5u;
                        std::memcpy(rdram + (physEntry + 16), &newFlag, sizeof(newFlag));

                        uint32_t seq = 0;
                        std::memcpy(&seq, rdram + (container & 0x1FFFFFFFu), sizeof(seq));
                        uint32_t v1 = seq + 1;
                        if (v1 == 1) {
                            std::memcpy(rdram + (container & 0x1FFFFFFFu), &v1, sizeof(v1));
                        } else {
                            const uint32_t newSeq = seq + 2;
                            v1 = 1;
                            std::memcpy(rdram + (container & 0x1FFFFFFFu), &newSeq, sizeof(newSeq));
                        }

                        std::memcpy(rdram + (physEntry + 20), &entry, sizeof(entry));
                        std::memcpy(rdram + (physEntry + 24), &v1, sizeof(v1));
                        allocatedEntry = entry;
                        break;
                    }
                }
            }
            ctx->r[2] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<int32_t>(allocatedEntry)));
            ctx->cop0_status |= 0x10000; // EnableInterrupts
            // Restore s0, s1, ra and sp += 48
            const uint32_t sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));
            uint64_t savedRa = 0, savedS1 = 0, savedS0 = 0;
            std::memcpy(&savedRa, rdram + ((sp + 32) & 0x1FFFFFFFu), sizeof(savedRa));
            std::memcpy(&savedS1, rdram + ((sp + 16) & 0x1FFFFFFFu), sizeof(savedS1));
            std::memcpy(&savedS0, rdram + ((sp + 0) & 0x1FFFFFFFu), sizeof(savedS0));
            ctx->r[31] = _mm_cvtsi64_si128(savedRa);
            ctx->r[17] = _mm_cvtsi64_si128(savedS1);
            ctx->r[16] = _mm_cvtsi64_si128(savedS0);
            ctx->r[29] = _mm_cvtsi64_si128(static_cast<int64_t>(sp + 48));
            ctx->pc = static_cast<uint32_t>(savedRa);
        };
        runtime.replaceFunction(0x004d6a40u, sifAllocPacketFallback4d6a40);

        // 0x004d7008: sceSifBindRpc(client in $a0, rpc_id in $a1, mode in $a2)
        static std::unordered_map<uint32_t, uint32_t> s_clientSidMap;
        static auto sifBindRpcStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            const uint32_t clientPtr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            const uint32_t rpcId = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
            const uint32_t mode = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[6]));
            RUNTIME_LOG("[game_overrides] sceSifBindRpc (0x4d7008) client=0x"
                        << std::hex << clientPtr << " sid=0x" << rpcId << " mode=0x" << mode << std::dec);

            s_clientSidMap[clientPtr] = rpcId;
            ps2_stubs::sceSifBindRpc(rdram, ctx, runtime);

            // Populate client and server descriptors in RDRAM for guest
            if (clientPtr != 0 && (clientPtr + 0x30u) <= 0x02000000) {
                const uint32_t dummyServer = 0x00a29800u + (clientPtr & 0x3f0u);
                const uint32_t dummyBuf = dummyServer + 0x40u;
                const uint32_t bufSize = 256;

                // Server descriptor:
                std::memset(rdram + (dummyServer & 0x1FFFFFFFu), 0, 64);
                std::memcpy(rdram + (dummyServer & 0x1FFFFFFFu), &rpcId, 4); // sid
                std::memcpy(rdram + ((dummyServer + 8) & 0x1FFFFFFFu), &dummyBuf, 4); // buf
                std::memcpy(rdram + ((dummyServer + 12) & 0x1FFFFFFFu), &bufSize, 4); // size
                std::memcpy(rdram + ((dummyServer + 20) & 0x1FFFFFFFu), &dummyBuf, 4); // cbuf
                std::memcpy(rdram + ((dummyServer + 24) & 0x1FFFFFFFu), &bufSize, 4); // size2
                std::memcpy(rdram + ((dummyServer + 28) & 0x1FFFFFFFu), &clientPtr, 4); // client

                // Client descriptor:
                std::memcpy(rdram + ((clientPtr + 0x14u) & 0x1FFFFFFFu), &dummyBuf, 4); // buf
                std::memcpy(rdram + ((clientPtr + 0x18u) & 0x1FFFFFFFu), &dummyBuf, 4); // cbuf
                std::memcpy(rdram + ((clientPtr + 0x24u) & 0x1FFFFFFFu), &dummyServer, 4); // server
            }

            ctx->r[2] = _mm_setzero_si128(); // return 0 (success)
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d7008u, sifBindRpcStub);

        // 0x004d714c: FUN_004d7008 (sceSifBindRpc) return stub -> jr $ra; addiu $sp, $sp, 0x70
        static auto returnStub4d714c = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x70);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d714cu, returnStub4d714c);

        // 0x004d71e8: sceSifCallRpc
        static auto sifCallRpcStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            uint32_t clientPtr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            uint32_t rpcNum = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));

            // Find sid from client descriptor or map
            uint32_t sid = 0;
            if ((clientPtr & 0x01FFFFFFu) + 0x28 <= 0x02000000u) {
                uint32_t serverPtr = 0;
                std::memcpy(&serverPtr, rdram + (clientPtr & 0x01FFFFFFu) + 0x24, 4);
                if ((serverPtr & 0x01FFFFFFu) + 4 <= 0x02000000u) {
                    std::memcpy(&sid, rdram + (serverPtr & 0x01FFFFFFu), 4);
                }
            }
            if (sid == 0) {
                auto it = s_clientSidMap.find(clientPtr);
                if (it != s_clientSidMap.end()) {
                    sid = it->second;
                }
            }

            // 2026-10-09 (c, Fase 1): HLE de los servicios del kernel IOP que el
            // recomp no emula (sifcmd: 0x80000592/0x8000059c; cdvdfsv: 0x80000006).
            // Sin esto, el transporte los devuelve 'unhandled' (rpc=0) y el EE
            // recibe ceros. Se responde éxito (v0=0) con recv en ceros y se
            // saltea el sceSifCallRpc real. Default ON; A/B con SWORD_RPC_HLE=0.
            {
                static const bool s_rpcHle = []() {
                    const char *e = std::getenv("SWORD_RPC_HLE");
                    return e != nullptr && *e != '0';
                }();
                const bool kernelSid = (sid == 0x80000592u || sid == 0x8000059Cu);
                const bool cdvdSid = (sid == 0x80000006u);
                if (s_rpcHle && (kernelSid || cdvdSid) && !(cdvdSid && rpcNum == 0xFFu)) {
                    const uint32_t recvPtr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[9]));
                    const uint32_t recvSz = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[10]));
                    if (recvPtr != 0u) {
                        const uint32_t phys = recvPtr & 0x01FFFFFFu;
                        const uint32_t n = (recvSz == 0u || recvSz > 0x1000u) ? 16u : recvSz;
                        if (phys + n <= 0x02000000u) {
                            // 2026-10-09: la referencia (savestate PCSX2 slot 01) muestra
                            // que sifcmd/cdvdfsv devuelven una STRUCT, no ceros:
                            //   {status, self, 0x8001f930 (workspace sifcmd), 0x10}
                            // El juego (FUN_004cf360) LEE esos campos y ramifica según ellos.
                            const uint32_t st = cdvdSid ? 0x20u : (sid == 0x8000059Cu ? 2u : 1u);
                            const uint32_t second = cdvdSid ? 2u : recvPtr;
                            const uint32_t words[4] = { st, second, 0x8001F930u, 0x10u };
                            const uint32_t w = (n < 16u) ? n : 16u;
                            std::memcpy(rdram + phys, words, w);
                            if (n > 16u) {
                                std::memset(rdram + phys + 16u, 0, n - 16u);
                            }
                        }
                    }
                    static int s_hleLog = 0;
                    if (++s_hleLog <= 10) {
                        RUNTIME_LOG("[rpc-hle] handled sid=0x" << std::hex << sid
                                    << " rpc=0x" << rpcNum << std::dec);
                    }
                    ctx->r[2] = _mm_cvtsi32_si128(0);
                    ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
                    return;
                }
            }

            const uint32_t savedA5Recv = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[9]));
            // Returns true if the call was fully handled (IOPHEAP early return).
            auto applyFixups = [&]() -> bool {
                // 2026-10-08 (A/B audio): con el IOP REAL cargado (SDRDRV/SD_CALL
                // físicos), el HLE de TYOSD puede estar TAPANDO al driver real.
                // SWORD_TYOSD_HLE=0 desactiva el HLE y deja pasar el RPC real
                // (experimento reversible, sin recompilar).
                static const bool s_tyosdHle = []() {
                    const char *e = std::getenv("SWORD_TYOSD_HLE");
                    return !(e != nullptr && *e == '0');
                }();
                // If CDVD (0x80000006) version query (0xFF), supply "3000" version signature
                if (sid == 0x80000006u && rpcNum == 0xFFu) {
                    uint32_t recvBuf = savedA5Recv;
                    if (recvBuf == 0) {
                        recvBuf = 0xa2dc00u;
                    }
                    uint32_t phys = recvBuf & 0x01FFFFFFu;
                    if (phys + 4 <= 0x02000000u) {
                        std::memcpy(rdram + phys, "3000", 4);
                    }
                    if ((0xa3de28u & 0x01FFFFFFu) + 4 <= 0x02000000u) {
                        std::memcpy(rdram + (0xa3de28u & 0x01FFFFFFu), "3000", 4);
                    }
                }
                else if (sid == 0x80000003u || clientPtr == 0x00A2DA40u) {
                    // SIF_RPC_IOPHEAP: sceSifAllocIopHeap(size) -> return valid pointer
                    uint32_t recvBuf = savedA5Recv;
                    if (recvBuf == 0) recvBuf = 0xa2da80u;
                    uint32_t phys = recvBuf & 0x01FFFFFFu;
                    if (phys + 4 <= 0x02000000u) {
                        uint32_t fakeIopAddr = 0x00080000u;
                        std::memcpy(rdram + phys, &fakeIopAddr, 4);
                    }
                    ctx->r[2] = _mm_cvtsi32_si128(0x00080000u);
                    ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
                    return true;
                }
                else if (s_tyosdHle && (sid == 0x573u || clientPtr == 0x00A44020u)) {
                    // Tokyo Sound Design (TYOSD) driver SD.BIN streaming.
                    // rpc 0x10000 = open "cdrom0:\SD.BIN;1" (send=path, recv 20B)
                    // rpc 0x20000/0x0 = poll done. rpc 0x30000 = streaming
                    // transfer request (send 640B desc, recv 352B).
                    // 2026-10-05: para 0x30000 en POST se salta el zero+status
                    // y se preserva la respuesta REAL del handler SDRDRV (ver
                    // rama hibrida abajo); en PRE se comporta como siempre.
                    // 2026-10-06w (callback?): log endFunction/endParameter
                    // UNA vez por rpcNum (¿el completado 44++ es un callback
                    // SIF que nunca corre? 0x627f4c no tiene callers directos).
                    {
                        static uint32_t s_loggedEnd[8] = {};
                        static int s_loggedEndN = 0;
                        bool seen = false;
                        for (int i = 0; i < s_loggedEndN; ++i)
                            if (s_loggedEnd[i] == rpcNum) { seen = true; break; }
                        if (!seen && s_loggedEndN < 8) {
                            s_loggedEnd[s_loggedEndN++] = rpcNum;
                            const uint32_t ef = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[11]));
                            const uint32_t sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));
                            uint32_t efS = 0, epS = 0;
                            const uint32_t sph = sp & 0x01FFFFFFu;
                            if (sph + 0x24 <= 0x02000000u) {
                                std::memcpy(&efS, rdram + sph + 0x1C, 4);
                                std::memcpy(&epS, rdram + sph + 0x20, 4);
                            }
                            RUNTIME_LOG("[TYOSD-endfn] rpc=0x" << std::hex << rpcNum << std::dec
                                        << " endFnReg=0x" << std::hex << ef << " endFnStk=0x" << efS
                                        << " endParStk=0x" << epS << std::dec);
                            // 2026-10-06x: ¿el runtime conoce el callback?
                            // Si hasFunction=false, el SifCallRpc real lo
                            // DESCARTA (sema en vez de callback) y el
                            // completado (44++/ei) nunca corre.
                            // 2026-10-06z2 (KICKSTART, REVERSIBLE): 627B50
                            // solo somete si 48>0; nada arranca el ciclo
                            // (48=44=0 naturales). Sembrar 48=1 UNA vez: si
                            // el ciclo se autosostiene (transfers+48 crecen
                            // solos, menú) → WIN con solo kickstart. Si un
                            // submit y park → ver su gate de completitud.
                            {
                                uint32_t cb = ef != 0u ? ef : efS;
                                if (cb != 0u) {
                                    RUNTIME_LOG("[TYOSD-endfn] hasFunction(0x" << std::hex << cb << ")="
                                                << (runtime->hasFunction(cb) ? "YES" : "NO") << std::dec);
                                }
                            }
                        }
                    }
                    static bool s_30000await = false;
                    const bool skipZero = (rpcNum == 0x30000u && s_30000await);
                    uint32_t recvBuf = savedA5Recv;
                    if (recvBuf == 0) recvBuf = 0x00A44900u;
                    const uint32_t sendBuf = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[7]));
                    uint32_t recvSize = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[10]));
                    if (recvSize == 0 || recvSize > 512) recvSize = 20;
                    const uint32_t rphys = recvBuf & 0x01FFFFFFu;
                    if (!skipZero && rphys + recvSize <= 0x02000000u) {
                        std::memset(rdram + rphys, 0, recvSize);
                        uint32_t one = 1;
                        std::memcpy(rdram + rphys, &one, 4); // status: done
                    }
                    if (rpcNum == 0x10000u) {
                        // Report real SD.BIN size so the EE sizes its ring.
                        char path[128] = {};
                        const uint32_t sph = (sendBuf + 4) & 0x01FFFFFFu;
                        if (sph + sizeof(path) <= 0x02000000u) {
                            std::memcpy(path, rdram + sph, sizeof(path) - 1);
                        }
                        std::error_code ec2;
                        const auto io = PS2Runtime::getIoPaths();
                        std::filesystem::path sd = io.cdRoot / "SD.BIN";
                        if (!std::filesystem::exists(sd, ec2)) {
                            sd = io.cdImage.parent_path() / "SD.BIN";
                        }
                        uint32_t fsize = 0, sectors = 0;
                        if (std::filesystem::exists(sd, ec2)) {
                            fsize = static_cast<uint32_t>(std::filesystem::file_size(sd, ec2));
                            sectors = (fsize + 2047) / 2048;
                        }
                        RUNTIME_LOG("[TYOSD] open path=" << path << " size=" << fsize);
                        if (rphys + 12 <= 0x02000000u) {
                            std::memcpy(rdram + rphys + 4, &fsize, 4);
                            std::memcpy(rdram + rphys + 8, &sectors, 4);
                        }
                    }
                    else if (rpcNum == 0x30000u) {
                        // 2026-10-04f (jugable-primero, híbrido): el
                        // passthrough puro reportaba fallo (v0=0) y el driver
                        // EE se quedaba en tormenta de polls 0x20000 (471k)
                        // sin reintentar la transferencia. Híbrido: pre deja
                        // correr el RPC real (SDRDRV ve el trabajo), post
                        // reporta éxito (v0=1) como hacía el HLE. La rama se
                        // ejecuta 2 veces por RPC (pre+post); alternancia
                        // estática empareja (EE monohilo).
                        // 2026-10-05: s_30000await empareja PRE/POST (el mismo
                        // static del prefijo): en PRE deja correr el RPC real.
                        static bool s_pre = true;
                        if (s_pre) {
                            s_pre = false;
                            s_30000await = true;
                            ps2x::iop::detail::g_tyosdTransferActive = true;
                            // 2026-10-05 (visual-primero): snapshot del vector
                            // de estado en rondas CON transferencia (primeras
                            // 3). Comparar contra rondas idle (spin) revela el
                            // trigger que distingue unas de otras.
                            {
                                static int s_vecCount = 0;
                                if (++s_vecCount <= 3) {
                                    const uint32_t gp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[28]));
                                    uint32_t v44 = 0, v48 = 0, v5d0 = 0, v5cf = 0, v604 = 0;
                                    uint32_t v610 = 0, v614 = 0, v630 = 0, v638 = 0, v63c = 0, v640 = 0;
                                    uint32_t cb = 0;
                                    const uint32_t g = gp & 0x01FFFFFFu;
                                    if (g + 0x644 <= 0x02000000u) {
                                        std::memcpy(&v44, rdram + g + 0x44, 4);
                                        std::memcpy(&v48, rdram + g + 0x48, 4);
                                        std::memcpy(&v5d0, rdram + g + 0x5D0, 1);
                                        std::memcpy(&v5cf, rdram + g + 0x5CF, 1);
                                        std::memcpy(&v604, rdram + g + 0x604, 1);
                                        std::memcpy(&v610, rdram + g + 0x610, 4);
                                        std::memcpy(&v614, rdram + g + 0x614, 4);
                                        std::memcpy(&v630, rdram + g + 0x630, 1);
                                        std::memcpy(&v638, rdram + g + 0x638, 4);
                                        std::memcpy(&v63c, rdram + g + 0x63C, 4);
                                        std::memcpy(&v640, rdram + g + 0x640, 1);
                                        std::memcpy(&cb, rdram + ((gp - 0x7A30u) & 0x01FFFFFFu), 4);
                                    }
                                    RUNTIME_LOG("[TYOSD-vec] n=" << s_vecCount
                                                << " 44=" << v44 << " 48=" << v48
                                                << " 5d0=" << (unsigned)v5d0 << " 5cf=" << (unsigned)v5cf
                                                << " 604=" << (unsigned)v604 << " 610=0x" << std::hex << v610
                                                << " 614=0x" << v614 << std::dec << " 630=" << (unsigned)v630
                                                << " 638=0x" << std::hex << v638 << " 63c=0x" << v63c
                                                << std::dec << " 640=" << (unsigned)v640
                                                << " cb=0x" << std::hex << cb << std::dec);
                                }
                            }
                            // 2026-10-05 (visual-primero): checksum del
                            // descriptor por llamada (primeras + cuando
                            // CAMBIA). Si el EE varía el request (p.ej. bit
                            // go/start), hay que verlo; si siempre idéntico,
                            // el EE espera algo externo para avanzar.
                            {
                                const uint32_t sphys = sendBuf & 0x01FFFFFFu;
                                static uint32_t s_lastSum = 0;
                                static int s_descCount = 0;
                                if (sphys + 640 <= 0x02000000u) {
                                    uint32_t sum = 0;
                                    for (int i = 0; i < 640; i += 4) {
                                        uint32_t w = 0;
                                        std::memcpy(&w, rdram + sphys + i, 4);
                                        sum = sum * 31u + w;
                                    }
                                    ++s_descCount;
                                    if (s_descCount <= 2 || sum != s_lastSum) {
                                        RUNTIME_LOG("[TYOSD-desc] n=" << s_descCount << " sum=0x" << std::hex << sum << std::dec << (sum != s_lastSum ? " CHANGED" : ""));
                                        s_lastSum = sum;
                                    }
                                }
                            }
                            RUNTIME_LOG("[TYOSD] rpc=0x30000 real corre (pre)");
                            // 2026-10-06i3 (goteo incremental, REVERSIBLE): el
                            // bloque de arriba quedó desactivado (44=65 daba
                            // sum>=0x41 → return 2 → el copy nunca corría).
                            // Protocolo real es incremental: el IOP entrega de
                            // a pocos (44=k), el EE copia (48+=k) hasta 65.
                            // Se alimenta 44=1 por poll hasta 48=65.
                            // 2026-10-06b (bulk-serve 4.6-4): volcado único del
                            // descriptor SEND para aprender su layout (¿dónde
                            // van offset/longitud en SD.BIN?). Una sola vez.
                            {
                                static bool s_sendDump = false;
                                if (!s_sendDump) {
                                    s_sendDump = true;
                                    const uint32_t mode = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[6]));
                                    const uint32_t ssize = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[8]));
                                    const uint32_t sphys = sendBuf & 0x01FFFFFFu;
                                    char line[256];
                                    int n = std::snprintf(line, sizeof(line),
                                        "[TYOSD-send] mode=%u ssize=%u sendBuf=0x%x recv=0x%x rsize=%u:",
                                        mode, ssize, sendBuf, recvBuf, recvSize);
                                    for (int i = 0; i < 10 && n < 220; ++i) {
                                        uint32_t w = 0xDEADBEEFu;
                                        if (sphys + (i + 1) * 4 <= 0x02000000u)
                                            std::memcpy(&w, rdram + sphys + i * 4, 4);
                                        n += std::snprintf(line + n, sizeof(line) - n, " %08x", w);
                                    }
                                    RUNTIME_LOG(line);
                                }
                            }
                            // 2026-10-06z2 (KICKSTART, REVERSIBLE, UNA vez):
                            // 627B50 solo somete si 48>0. Sembrar 48=1 para
                            // arrancar el ciclo; el resto corre real.
                            {
                                static bool s_kick = false;
                                if (!s_kick) {
                                    s_kick = true;
                                    const uint32_t gp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[28]));
                                    const uint32_t p48 = (gp + 0x48u) & 0x01FFFFFFu;
                                    if (p48 + 4 <= 0x02000000u) {
                                        const uint32_t one = 1;
                                        std::memcpy(rdram + p48, &one, 4);
                                        RUNTIME_LOG("[TYOSD] kickstart 48=1");
                                    }
                                }
                            }
                            // 2026-10-09 (b, Fase 1): servir datos REALES de
                            // work/elf/SD.BIN en el streaming (rpc=0x30000) en
                            // vez de ceros. Offset incremental. Default OFF;
                            // activar con SWORD_SD_SERVE=1.
                            {
                                static const bool s_sdServe = std::getenv("SWORD_SD_SERVE") != nullptr;
                                if (s_sdServe) {
                                    static FILE *s_fp = nullptr;
                                    static uint64_t s_off = 0;
                                    if (s_fp == nullptr) {
                                        s_fp = std::fopen("work/elf/SD.BIN", "rb");
                                        RUNTIME_LOG("[sd-serve] open SD.BIN " << (s_fp ? "OK" : "FAIL"));
                                    }
                                    if (s_fp != nullptr) {
                                        const uint32_t rp = recvBuf & 0x01FFFFFFu;
                                        const uint32_t n = (recvSize == 0u || recvSize > 0x2000u) ? 352u : recvSize;
                                        if (rp + n <= 0x02000000u) {
                                            std::fseek(s_fp, static_cast<long>(s_off), SEEK_SET);
                                            const size_t rd = std::fread(rdram + rp, 1, n, s_fp);
                                            if (rd < n) std::memset(rdram + rp + rd, 0, n - rd);
                                            s_off += rd;
                                            static int s_sdLog = 0;
                                            if (++s_sdLog <= 5 || s_sdLog % 200 == 1) {
                                                RUNTIME_LOG("[sd-serve] n=" << s_sdLog << " off=0x"
                                                            << std::hex << (s_off - rd) << std::dec
                                                            << " bytes=" << rd);
                                            }
                                        }
                                    }
                                }
                            }
                            return false;
                        }
                        s_pre = true;
                        s_30000await = false;
                        ps2x::iop::detail::g_tyosdTransferActive = false;
                        ctx->r[2] = _mm_cvtsi32_si128(1);
                        ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
                        // 2026-10-06b (bulk-serve 4.6-4, TRIAGE): el reply real
                        // son ceros y el EE reintenta el bloque 1 eternamente
                        // (comando 16B constante). hypothesis: el EE espera
                        // status-done por bloque en recv (352B = 32 + 40x8).
                        // Se reporta todo-listo: word0=1 + resto en 1.
                        // Si avanza → breakthrough; si crashea → el pc dice
                        // qué layout esperaba; si igual → no lee recv.
                        {
                            const uint32_t rp = recvBuf & 0x01FFFFFFu;
                            if (rp + 352 <= 0x02000000u) {
                                const uint32_t one = 1;
                                for (int i = 0; i < 88; ++i)
                                    std::memcpy(rdram + rp + i * 4, &one, 4);
                            }
                            static int s_doneLog = 0;
                            if (++s_doneLog <= 2)
                                RUNTIME_LOG("[TYOSD] rpc=0x30000 all-done (TRIAGE bulk)");
                        }
                        // 2026-10-06f (completado one-shot, REVERSIBLE): simular
                        // "los 40 bloques llegaron" UNA vez: 44 = 40. El
                        // secuenciador real (caller 0x627bc0) copia 44 bloques
                        // (0xA44100 → 0xA44500+(48+i)*16), hace 48 += 44 y
                        // 44 = 0. Polls siguientes ven 44 = 0 → rama done.
                        // Si avanza (pcs/gif/menú) → breakthrough: luego se
                        // implementa el completado por-bloque real. Si nada →
                        // el gate está en otro lado (w2/tabla). Si crashea →
                        // el pc dice qué esperaba el copy-loop.
                        {
                            static bool s_completed = false;
                            if (!s_completed) {
                                s_completed = true;
                                const uint32_t gp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[28]));
                                const uint32_t p44 = (gp + 0x44u) & 0x01FFFFFFu;
                                const uint32_t forty = 40;
                                if (p44 + 4 <= 0x02000000u) {
                                    uint32_t before = 0;
                                    std::memcpy(&before, rdram + p44, 4);
                                    std::memcpy(rdram + p44, &forty, 4);
                                    RUNTIME_LOG("[TYOSD] rpc=0x30000 one-shot complete: 44 " << before << " -> 40");
                                }
                            }
                        }
                        return true;
                    }
                    else if (rpcNum == 0x0u) {
                        // Poll de streaming + principio jugable-primero
                        // (2026-10-04): el productor re-encola porque la
                        // posición nunca avanza (recv en ceros). Se simula
                        // progreso de reproducción (sin datos reales): pos
                        // += 16KB por poll con wrap en tamaño de SD.BIN.
                        // Si el productor observa avance, deja de reintentar
                        // y el juego sigue (silencioso) hacia el menú.
                        // TRIAGE, no fix real.
                        // 2026-10-06g (pos-gate, REVERSIBLE): salto al final SIN
                        // wrap. Si el secuenciador espera pos >= fin-de-rango,
                        // completa en pocos polls. Si esperaba avance gradual
                        // (overshoot = "no son mis datos"), esto lo rompe y se
                        // verá retroceso (más reintentos) → revertir y avanzar
                        // 16KB por bloque completado en su lugar.
                        {
                            const uint32_t rphys = recvBuf & 0x01FFFFFFu;
                            static const uint32_t kSdSize = 715327488u; // SD.BIN real
                            static uint32_t fakePos = kSdSize - 65536u; // 64KB del fin
                            if (fakePos < kSdSize) {
                                fakePos += 16384u;
                                if (fakePos >= kSdSize) fakePos = kSdSize - 1u; // clamp, NO wrap
                            }
                            if (rphys + 12 <= 0x02000000u) {
                                std::memcpy(rdram + rphys + 4, &fakePos, 4);
                                const uint32_t sectors = fakePos / 2048u;
                                std::memcpy(rdram + rphys + 8, &sectors, 4);
                            }
                            static int posLog = 0;
                            if (++posLog % 200 == 1) {
                                RUNTIME_LOG("[TYOSD-pos] fakePos=" << fakePos);
                            }
                        }
                        // 2026-10-06v (BASELINE): fakes DESACTIVADOS (goteo y
                        // 48-directo). Solo log de valores naturales para
                        // observar el estado estacionario REAL y diseñar UN
                        // fix coherente lado IOP.
                        {
                            const uint32_t gp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[28]));
                            const uint32_t p44 = (gp + 0x44u) & 0x01FFFFFFu;
                            const uint32_t p48 = (gp + 0x48u) & 0x01FFFFFFu;
                            if (p44 + 4 <= 0x02000000u && p48 + 4 <= 0x02000000u) {
                                uint32_t c44 = 0, c48 = 0;
                                std::memcpy(&c44, rdram + p44, 4);
                                std::memcpy(&c48, rdram + p48, 4);
                                static int baseLog = 0;
                                if (++baseLog % 60 == 1) {
                                    RUNTIME_LOG("[TYOSD-base] 44=" << c44 << " 48=" << c48);
                                }
                                // 2026-10-07i (muestreo a/b del spin 6e4d98):
                                // gates gp+5ec/610/614/638/63c + FLIP detect.
                                // 2026-10-07k: + callback (gp-7a30) y flag
                                // (gp+630): el loop solo sale si el jalr no
                                // vuelve o s0==0; si cb==NULL → loop infinito
                                // por diseño O por falta de registro.
                                {
                                    const uint32_t ao[9] = {0x5ECu, 0x610u, 0x614u, 0x638u, 0x63Cu, 0x630u, 0xFFFFFF86u /*-0x7a30*/, 0x604u, 0x5CFu};
                                    uint32_t av[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
                                    for (int i = 0; i < 9; ++i) {
                                        const uint32_t a = (gp + ao[i]) & 0x01FFFFFFu;
                                        if (i == 5 || i == 7 || i == 8) { // lbu bytes
                                            uint8_t b = 0;
                                            if (a + 1 <= 0x02000000u) std::memcpy(&b, rdram + a, 1);
                                            av[i] = b;
                                        } else if (a + 4 <= 0x02000000u) std::memcpy(&av[i], rdram + a, 4);
                                    }
                                    static int s_ab = 0;
                                    static uint32_t s_pa = 0, s_pb = 0;
                                    const bool chg = (s_ab > 0) && (av[1] != s_pa || av[2] != s_pb);
                                    if (++s_ab % 60 == 1 || chg) {
                                        RUNTIME_LOG("[TYOSD-ab] gp=0x" << std::hex << gp << " 5ec=0x" << av[0] << " a=0x" << av[1] << " b=0x" << av[2] << " 638=0x" << av[3] << " 63c=0x" << av[4] << " f630=0x" << av[5] << " cb=0x" << av[6] << " f604=0x" << av[7] << " f5cf=0x" << av[8] << std::dec << (chg ? " FLIP" : ""));
                                    }
                                    s_pa = av[1]; s_pb = av[2];
                                }
                                // 2026-10-07 (ver imagen): captura periódica
                                // del frame presentado (¿logo? ¿menú? ¿negro?).
                                {
                                    static int s_shot = 0;
                                    if (++s_shot % 240 == 1) {
                                        std::vector<uint8_t> rgba;
                                        uint32_t w = 0, h = 0;
                                        runtime->gs().latchHostPresentationFrame();
                                        if (runtime->gs().copyLatchedHostPresentationFrame(rgba, w, h) && !rgba.empty()) {
                                            char fn[128];
                                            std::snprintf(fn, sizeof(fn), "/tmp/opencode/shot_%d.bmp", s_shot);
                                            std::ofstream out(fn, std::ios::binary);
                                            if (out) {
                                                const uint32_t rowPad = (4 - (w * 3) % 4) % 4;
                                                const uint32_t imgSize = (w * 3 + rowPad) * h;
                                                const uint32_t fileSize = 54 + imgSize;
                                                uint8_t hdr[54] = {};
                                                hdr[0] = 'B'; hdr[1] = 'M';
                                                std::memcpy(hdr + 2, &fileSize, 4);
                                                const uint32_t off = 54;
                                                std::memcpy(hdr + 10, &off, 4);
                                                const uint32_t dib = 40;
                                                std::memcpy(hdr + 14, &dib, 4);
                                                std::memcpy(hdr + 18, &w, 4);
                                                std::memcpy(hdr + 22, &h, 4);
                                                const uint16_t planes = 1, bpp = 24;
                                                std::memcpy(hdr + 26, &planes, 2);
                                                std::memcpy(hdr + 28, &bpp, 2);
                                                out.write(reinterpret_cast<char*>(hdr), 54);
                                                std::vector<uint8_t> row(w * 3 + rowPad);
                                                for (int y = h - 1; y >= 0; --y) {
                                                    for (uint32_t x = 0; x < w; ++x) {
                                                        row[x * 3 + 0] = rgba[(y * w + x) * 4 + 2];
                                                        row[x * 3 + 1] = rgba[(y * w + x) * 4 + 1];
                                                        row[x * 3 + 2] = rgba[(y * w + x) * 4 + 0];
                                                    }
                                                    out.write(reinterpret_cast<char*>(row.data()), row.size());
                                                }
                                                RUNTIME_LOG("[shot] saved " << fn << " " << w << "x" << h);
                                            }
                                        } else {
                                            static bool s_shotFail = false;
                                            if (!s_shotFail) {
                                                s_shotFail = true;
                                                RUNTIME_LOG("[shot] latch/copy failed (empty)");
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        static bool dumpedPoll0 = false;
                        if (!dumpedPoll0) {
                            dumpedPoll0 = true;
                            // 2026-10-06k: ¿recv+4 == gp+0x44 (aliasing)?
                            // Si el reply del poll cae sobre el contador 44,
                            // el protocolo es directo y fakePos lo corrompía.
                            {
                                const uint32_t gp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[28]));
                                const uint32_t ra = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
                                RUNTIME_LOG("[TYOSD-poll] recv=0x" << std::hex << recvBuf << " rsize=" << std::dec << recvSize
                                            << " gp=0x" << std::hex << gp << " ra=0x" << ra << std::dec);
                            }
                            dumpedPoll0 = true;
                            const uint32_t pphys = recvBuf & 0x01FFFFFFu;
                            if (pphys + 64 <= 0x02000000u) {
                                char line[160];
                                int n = std::snprintf(line, sizeof(line), "[TYOSD-poll]");
                                for (int i = 0; i < 8 && n < 140; ++i) {
                                    uint32_t w = 0;
                                    std::memcpy(&w, rdram + pphys + i * 4, 4);
                                    n += std::snprintf(line + n, sizeof(line) - n, " %08x", w);
                                }
                                RUNTIME_LOG(line);
                            }
                            const uint32_t gp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[28]));
                            uint32_t c44 = 0, c48 = 0;
                            std::memcpy(&c44, rdram + ((gp + 0x44u) & 0x01FFFFFFu), 4);
                            std::memcpy(&c48, rdram + ((gp + 0x48u) & 0x01FFFFFFu), 4);
                            RUNTIME_LOG("[TYOSD-poll] gp44=" << c44 << " gp48=" << c48);
                        }
                        RUNTIME_LOG("[TYOSD] rpc=0x0 ack done");
                    }
                    else if (rpcNum == 0x20000u) {
                        // 2026-10-04g (jugable-primero): tormenta 0x20000
                        // (~8000/s): el driver espera un flag "ready" en recv
                        // que devolvemos en ceros. Prueba: todo-uno en
                        // recv[1..7] (352 B). Si se rompe la tormenta, el flag
                        // está ahí y se biseca; si no, está en otro lado.
                        // TRIAGE, no fix real.
                        const uint32_t rphys = recvBuf & 0x01FFFFFFu;
                        if (rphys + 32 <= 0x02000000u) {
                            static const uint32_t kAllDone = 0xFFFFFFFFu;
                            for (int w = 1; w < 8; ++w) {
                                std::memcpy(rdram + rphys + w * 4, &kAllDone, 4);
                            }
                        }
                        RUNTIME_LOG("[TYOSD] rpc=0x20000 all-done (TRIAGE)");
                    }
                    else {
                        RUNTIME_LOG("[TYOSD] rpc=0x" << std::hex << rpcNum << std::dec << " ack done");
                    }
                    // El EE (0x627afc: bnez v0 -> éxito) reintenta mientras v0==0.
                    // Devolvemos 1 y no llamamos al SifCallRpc real (unhandled).
                    ctx->r[2] = _mm_cvtsi32_si128(1);
                    ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
                    return true;
                }
                else if (sid == 0x80000592u || sid == 0x80000006u || sid == 0x8000059cu) {
                    uint32_t recvBuf = savedA5Recv;
                    // 2026-10-10 (SWORD_RPC_FILL): el real devuelve una respuesta de 16-24 B
                    // (leida de los savestates en 0xA31B80). Por defecto se dejan ceros (baseline).
                    static const bool s_rpcFill = std::getenv("SWORD_RPC_FILL") != nullptr;
                    if (recvBuf != 0) {
                        uint32_t phys = recvBuf & 0x01FFFFFFu;
                        if (phys + 24u <= 0x02000000u) {
                            if (s_rpcFill) {
                                static const uint32_t kResp[6] = {0x00000008u, 0u, 0u, 0xFFFF0001u, 0x0000FFFFu, 0x00000001u};
                                std::memcpy(rdram + phys, kResp, sizeof(kResp));
                            } else {
                                std::memset(rdram + phys, 0, 24u);
                            }
                        }
                    }
                }
                return false;
            };

            // Advance PC to $ra and set v0 BEFORE calling sceSifCallRpc: a NOWAIT callback
            // suspends this context, so parent.pc must already point to $ra on resume.
            ctx->r[2] = _mm_setzero_si128();
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
            applyFixups();

            ps2_syscalls::sceSifCallRpc(rdram, ctx, runtime);

            // The runtime's unhandled-RPC fallback may zero recvBuf; reapply afterwards.
            if (applyFixups()) {
                return;
            }
            ctx->r[2] = _mm_setzero_si128(); // return 0 (success)
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d71e8u, sifCallRpcStub);

        // 0x004d73e0: FUN_004d71e8 (sceSifCallRpc) return stub -> jr $ra; addiu $sp, $sp, 0xc0
        static auto returnStub4d73e0 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0xc0);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d73e0u, returnStub4d73e0);

        // 0x004d67b0 / 0x004d6854 / 0x004d685c: sceSifWriteBackDCache -> no-op cache flush on PC, return to $ra
        static auto sifWriteBackDCacheStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d67b0u, sifWriteBackDCacheStub);
        runtime.replaceFunction(0x004d6854u, sifWriteBackDCacheStub);
        runtime.replaceFunction(0x004d685cu, sifWriteBackDCacheStub);

        // Generic return stub for truncated function epilogues (jr $ra + delay slot)
        static auto swordOfEtheriaGenericReturnStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t pc = ctx->pc;
            const uint32_t phys = pc & 0x01FFFFFFu;
            if (phys + 8 <= 0x02000000u) {
                uint32_t insn2 = 0;
                std::memcpy(&insn2, rdram + phys + 4, 4);

                const uint32_t op = insn2 >> 26;
                const uint32_t rs = (insn2 >> 21) & 0x1fu;
                const uint32_t rt = (insn2 >> 16) & 0x1fu;
                const int16_t simm = static_cast<int16_t>(insn2 & 0xFFFFu);

                // 1) addiu $sp, $sp, imm (0x27bdXXXX)
                if (op == 9u && rs == 29u && rt == 29u) {
                    ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + simm);
                }
                // 2) daddu $v0, $a0, $zero (0x0080102d)
                else if (insn2 == 0x0080102du) {
                    ctx->r[2] = ctx->r[4];
                }
                // 3) addiu $v0, $zero, imm (0x2402XXXX)
                else if (op == 9u && rs == 0u && rt == 2u) {
                    ctx->r[2] = _mm_cvtsi32_si128(simm);
                }
                // 3b) addiu $v0, $v0, imm (0x2442XXXX)
                else if (op == 9u && rs == 2u && rt == 2u) {
                    const int32_t cur = _mm_cvtsi128_si32(ctx->r[2]);
                    ctx->r[2] = _mm_cvtsi32_si128(cur + simm);
                }
                // 4) daddu $v0, $zero, $zero (0x0000102d)
                else if (insn2 == 0x0000102du) {
                    ctx->r[2] = _mm_setzero_si128();
                }
                // 5) sw $rt, imm($rs) (op == 43)
                else if (op == 43u) {
                    const uint32_t baseAddr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[rs])) + simm;
                    const uint32_t val = (rt == 0u) ? 0u : static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[rt]));
                    const uint32_t effPhys = baseAddr & 0x01FFFFFFu;
                    if (effPhys + 4 <= 0x02000000u) {
                        std::memcpy(rdram + effPhys, &val, 4);
                    }
                }
                // 6) lw $rt, imm($rs) (op == 35)
                else if (op == 35u && rt != 0u) {
                    const uint32_t baseAddr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[rs])) + simm;
                    const uint32_t effPhys = baseAddr & 0x01FFFFFFu;
                    if (effPhys + 4 <= 0x02000000u) {
                        uint32_t val = 0;
                        std::memcpy(&val, rdram + effPhys, 4);
                        ctx->r[rt] = _mm_cvtsi32_si128(static_cast<int32_t>(val));
                    }
                }
            }
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };

        for (size_t i = 0; i < s_swordOfEtheriaReturnPcsCount; ++i) {
            runtime.replaceFunction(s_swordOfEtheriaReturnPcs[i], swordOfEtheriaGenericReturnStub);
        }

        const uint32_t extraReturnPcs[] = {
            0x004cd100u, 0x004cd184u, 0x004cdb3cu, 0x004cdc44u,
            0x004cdeccu, 0x004ce338u, 0x004ce420u, 0x004ce590u,
            0x004ce79cu, 0x004ce9e0u, 0x004cea60u, 0x004ceb30u,
            0x004cf358u, 0x004cf858u, 0x004cfd28u, 0x004cfe3cu,
            0x004cfe74u, 0x004cfea4u, 0x004cfed4u, 0x004cff10u
        };
        for (uint32_t pc : extraReturnPcs) {
            runtime.replaceFunction(pc, swordOfEtheriaGenericReturnStub);
        }

        // 0x004ceb38 / 0x004cee38: sceCdSearchFile(sceCdlFILE *file, const char *name)
        static auto cdSearchFileStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            const uint32_t fileAddr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            const uint32_t pathAddr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
            RUNTIME_LOG("[game_overrides] sceCdSearchFile (0x4ceb38/0x4cee38) file=0x"
                        << std::hex << fileAddr << " pathAddr=0x" << pathAddr << std::dec);
            ps2_stubs::sceCdSearchFile(rdram, ctx, runtime);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004ceb38u, cdSearchFileStub);
        runtime.replaceFunction(0x004cee38u, cdSearchFileStub);

        // 0x004cfa80: sceCdRead(uint32_t lsn, uint32_t sectors, void *buf, sceCdRMode *mode)
        static auto cdReadStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            const uint32_t lsn = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            const uint32_t sectors = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
            const uint32_t buf = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[6]));
            const uint32_t ra = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
            RUNTIME_LOG("[game_overrides] sceCdRead (0x4cfa80) lsn=" << lsn << " sectors=" << sectors << " buf=0x"
                        << std::hex << buf << " ra=0x" << ra << std::dec);
            ps2_stubs::sceCdRead(rdram, ctx, runtime);
            // TRIAGE-CD 2026-10-08: verificar que los datos del disco lleguen
            // al guest (hipótesis "escena corrupta" => sin draws). Vuelca los
            // primeros 32 B del destino para comparar contra el ISO.
            {
                static int s_dumps = 0;
                const uint32_t phys = buf & 0x01FFFFFFu;
                if (s_dumps < 12 && buf != 0u && phys + 32u <= 0x02000000u) {
                    ++s_dumps;
                    char line[200];
                    int n = std::snprintf(line, sizeof(line),
                                         "[cdread-data] lsn=%u sec=%u buf=0x%x:",
                                         lsn, sectors, buf);
                    for (int i = 0; i < 32 && n < 180; ++i) {
                        n += std::snprintf(line + n, sizeof(line) - n, " %02x",
                                           rdram[phys + static_cast<uint32_t>(i)]);
                    }
                    RUNTIME_LOG(line);
                }
            }
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004cfa80u, cdReadStub);

        // 0x004cf048: sceCdSync(int mode)
        static auto cdSyncStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            ps2_stubs::sceCdSync(rdram, ctx, runtime);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004cf048u, cdSyncStub);

        // 0x004cf860: sceCdDiskReady(int mode) -> SCECdComplete (2).
        // 2026-10-11: el cuerpo real hace PollSema del sema de libcdvd (0x8720b0) y una RPC a
        // cdvdfsv (sid 0x8000059c). Como sceCdRead/sceCdSync son HLE, ese sema nunca vuelve y
        // la función devuelve 6 (SCECdNotReady) para siempre. El hilo loader de streaming
        // (0x628640, estado 3) reintenta indefinidamente con gp+0x58=1 => la escena nunca pasa
        // de `draw` a `menú`. A/B con SWORD_CD_DISKREADY_REAL=1.
        if (std::getenv("SWORD_CD_DISKREADY_REAL") == nullptr)
        {
            static auto cdDiskReadyStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
                ps2_stubs::sceCdDiskReady(rdram, ctx, runtime);
                ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
            };
            runtime.replaceFunction(0x004cf860u, cdDiskReadyStub);
        }

        // 0x004cf648: CDVD internal queue cleanup helper -> return 0
        static auto cdCleanupStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->r[2] = _mm_setzero_si128();
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004cf648u, cdCleanupStub);

        // 0x004ceac0: FUN_004ceac0 (PAD init command handler) -> mark initialized & return 1
        static auto padInitCmdHandlerStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t zero = 0;
            const uint32_t one = 1;
            if (0x8720a4u + 4 <= 0x02000000) {
                std::memcpy(rdram + 0x8720a4u, &zero, 4);
            }
            if (0x8720c8u + 4 <= 0x02000000) {
                std::memcpy(rdram + 0x8720c8u, &one, 4);
            }
            ctx->r[2] = _mm_cvtsi32_si128(1); // return 1 (success)
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004ceac0u, padInitCmdHandlerStub);

        // 0x004cfc60: FUN_004cfc60 (PAD port init/open) -> store mode and return 1
        static auto padPortInitStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            uint32_t mode = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            if (0x8736c0u + 4 <= 0x02000000) {
                std::memcpy(rdram + 0x8736c0u, &mode, 4);
            }
            ctx->r[2] = _mm_cvtsi32_si128(1); // return 1
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004cfc60u, padPortInitStub);

        // 0x004d36b0: sceSifGetReg (syscall 0x7A / 122) -> return SIF_REG_SMFLAG ready mask
        static auto sifGetRegStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            const uint32_t reg = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            if (reg == 4u) {
                // SMFLAG: return IOP/MC/SIF ready mask (bit 18 set: 0x00040000 | 0x00020000)
                ctx->r[2] = _mm_cvtsi32_si128(0x00040000 | 0x00020000);
            } else {
                ctx->r[2] = _mm_setzero_si128();
            }
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d36b0u, sifGetRegStub);

        // 0x004db550: FUN_004db550 (mcInit) -> bind MC RPC client (0xa2da40), return 0
        static auto mcInitStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t clientPtr = 0x00a2da40u;
            const uint32_t rpcId = 0x80000003u;
            const uint32_t dummyServer = 0x00a29700u;
            const uint32_t dummyBuf = 0x00a29780u;
            const uint32_t bufSize = 256;

            // Server descriptor
            std::memset(rdram + (dummyServer & 0x1FFFFFFFu), 0, 64);
            std::memcpy(rdram + (dummyServer & 0x1FFFFFFFu), &rpcId, 4);
            std::memcpy(rdram + ((dummyServer + 8) & 0x1FFFFFFFu), &dummyBuf, 4);
            std::memcpy(rdram + ((dummyServer + 12) & 0x1FFFFFFFu), &bufSize, 4);
            std::memcpy(rdram + ((dummyServer + 20) & 0x1FFFFFFFu), &dummyBuf, 4);
            std::memcpy(rdram + ((dummyServer + 24) & 0x1FFFFFFFu), &bufSize, 4);
            std::memcpy(rdram + ((dummyServer + 28) & 0x1FFFFFFFu), &clientPtr, 4);

            // Client descriptor
            std::memcpy(rdram + ((clientPtr + 0x14u) & 0x1FFFFFFFu), &dummyBuf, 4);
            std::memcpy(rdram + ((clientPtr + 0x18u) & 0x1FFFFFFFu), &dummyBuf, 4);
            std::memcpy(rdram + ((clientPtr + 0x24u) & 0x1FFFFFFFu), &dummyServer, 4);

            const uint32_t zero = 0;
            if (0x873e28u + 4 <= 0x02000000) {
                std::memcpy(rdram + 0x873e28u, &zero, 4);
            }

            ctx->r[2] = _mm_setzero_si128(); // return 0 (success)
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004db550u, mcInitStub);

        // 0x004db5a4: FUN_004db550 continuation stub
        static auto returnStub4db5a4 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            uint32_t sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));
            uint32_t phys = sp & 0x01FFFFFFu;
            if (phys + 0x28 <= 0x02000000u) {
                uint64_t raVal = 0, s1Val = 0, s0Val = 0;
                std::memcpy(&raVal, rdram + phys + 0x20, 8);
                std::memcpy(&s1Val, rdram + phys + 0x10, 8);
                std::memcpy(&s0Val, rdram + phys + 0x00, 8);
                ctx->r[31] = _mm_cvtsi64_si128(raVal);
                ctx->r[17] = _mm_cvtsi64_si128(s1Val);
                ctx->r[16] = _mm_cvtsi64_si128(s0Val);
            }
            ctx->r[29] = _mm_cvtsi64_si128(sp + 0x30);
            ctx->r[2] = _mm_setzero_si128(); // return 0
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004db5a4u, returnStub4db5a4);

        // 0x004dbc30: FUN_004dbc30 (mc aux reset) -> set [0x873e30] = -1, zero [0xa3de28], return 0
        static auto mcResetStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t neg1 = 0xFFFFFFFFu;
            const uint32_t zero = 0;
            if (0x873e30u + 4 <= 0x02000000) {
                std::memcpy(rdram + 0x873e30u, &neg1, 4);
            }
            if ((0xa3de28u & 0x01FFFFFFu) + 4 <= 0x02000000) {
                std::memcpy(rdram + (0xa3de28u & 0x01FFFFFFu), &zero, 4);
            }
            ctx->r[2] = _mm_setzero_si128(); // return 0
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004dbc30u, mcResetStub);

        // 0x004d83c0: FUN_004d83c0 (mc aux init) -> set [0x873e0c] = 0, zero [0xa3d968], return 0
        static auto mcInitAuxStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t zero = 0;
            if (0x873e0cu + 4 <= 0x02000000) {
                std::memcpy(rdram + 0x873e0cu, &zero, 4);
            }
            if ((0xa3d968u & 0x01FFFFFFu) + 4 <= 0x02000000) {
                std::memcpy(rdram + (0xa3d968u & 0x01FFFFFFu), &zero, 4);
            }
            ctx->r[2] = _mm_setzero_si128(); // return 0
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d83c0u, mcInitAuxStub);

        // 0x004c6818: strcat(char *dest, const char *src) -> returns dest
        static auto strcatStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t destAddr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            const uint32_t srcAddr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));

            const uint32_t destPhys = destAddr & 0x01FFFFFFu;
            const uint32_t srcPhys = srcAddr & 0x01FFFFFFu;

            if (destPhys < 0x02000000u && srcPhys < 0x02000000u) {
                char *destStr = reinterpret_cast<char *>(rdram + destPhys);
                const char *srcStr = reinterpret_cast<const char *>(rdram + srcPhys);
                size_t destLen = 0;
                while (destPhys + destLen < 0x02000000u && destStr[destLen] != '\0') {
                    destLen++;
                }
                size_t i = 0;
                while (destPhys + destLen + i < 0x02000000u && srcPhys + i < 0x02000000u) {
                    destStr[destLen + i] = srcStr[i];
                    if (srcStr[i] == '\0') break;
                    i++;
                }
            }

            ctx->r[2] = ctx->r[4]; // return dest in $v0
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004c6818u, strcatStub);

        // 0x0051f328: FUN_0051f220 early return stub -> jr $ra; addiu $sp, $sp, 0x150
        static auto returnStub51f328 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x150);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x0051f328u, returnStub51f328);

        // 0x004c71d8: strncpy(char *dest, const char *src, size_t n) -> returns dest
        static auto strncpyStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t destAddr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            const uint32_t srcAddr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
            const uint32_t n = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[6]));

            const uint32_t destPhys = destAddr & 0x01FFFFFFu;
            const uint32_t srcPhys = srcAddr & 0x01FFFFFFu;

            if (destPhys < 0x02000000u) {
                char *destStr = reinterpret_cast<char *>(rdram + destPhys);
                const char *srcStr = (srcPhys < 0x02000000u) ? reinterpret_cast<const char *>(rdram + srcPhys) : nullptr;
                size_t i = 0;
                bool nullFound = false;
                while (i < n && (destPhys + i) < 0x02000000u) {
                    if (!nullFound && srcStr && (srcPhys + i) < 0x02000000u) {
                        destStr[i] = srcStr[i];
                        if (srcStr[i] == '\0') {
                            nullFound = true;
                        }
                    } else {
                        destStr[i] = '\0';
                    }
                    i++;
                }
            }

            ctx->r[2] = ctx->r[4]; // return dest in $v0
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004c71d8u, strncpyStub);

        // 0x004c6948: strchr(const char *s, int c) -> returns pointer in $v0
        static auto strchrStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t sAddr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            const char ch = static_cast<char>(_mm_cvtsi128_si32(ctx->r[5]));
            const uint32_t sPhys = sAddr & 0x01FFFFFFu;

            uint32_t result = 0;
            if (sPhys < 0x02000000u) {
                const char *s = reinterpret_cast<const char *>(rdram + sPhys);
                const size_t maxLen = 0x02000000u - sPhys;
                for (size_t i = 0; i < maxLen; ++i) {
                    if (s[i] == ch) {
                        result = sAddr + static_cast<uint32_t>(i);
                        break;
                    }
                    if (s[i] == '\0') break;
                }
            }
            ctx->r[2] = _mm_cvtsi32_si128(static_cast<int32_t>(result));
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004c6948u, strchrStub);

        // 0x004c6ad8: strcmp(const char *s1, const char *s2) -> returns int in $v0
        static auto strcmpStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t s1Addr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            const uint32_t s2Addr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
            const uint32_t s1Phys = s1Addr & 0x01FFFFFFu;
            const uint32_t s2Phys = s2Addr & 0x01FFFFFFu;

            int diff = 0;
            if (s1Phys < 0x02000000u && s2Phys < 0x02000000u) {
                const uint8_t *u1 = rdram + s1Phys;
                const uint8_t *u2 = rdram + s2Phys;
                size_t i = 0;
                while ((s1Phys + i) < 0x02000000u && (s2Phys + i) < 0x02000000u) {
                    if (u1[i] != u2[i] || u1[i] == '\0') {
                        diff = static_cast<int>(u1[i]) - static_cast<int>(u2[i]);
                        break;
                    }
                    i++;
                }
            }
            ctx->r[2] = _mm_cvtsi32_si128(diff);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004c6ad8u, strcmpStub);

        // 0x004c6c20: strcpy(char *dest, const char *src) -> returns dest in $v0
        static auto strcpyStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t destAddr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            const uint32_t srcAddr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
            const uint32_t destPhys = destAddr & 0x01FFFFFFu;
            const uint32_t srcPhys = srcAddr & 0x01FFFFFFu;

            if (destPhys < 0x02000000u && srcPhys < 0x02000000u) {
                char *destStr = reinterpret_cast<char *>(rdram + destPhys);
                const char *srcStr = reinterpret_cast<const char *>(rdram + srcPhys);
                size_t i = 0;
                while ((destPhys + i) < 0x02000000u && (srcPhys + i) < 0x02000000u) {
                    destStr[i] = srcStr[i];
                    if (srcStr[i] == '\0') break;
                    i++;
                }
            }
            ctx->r[2] = ctx->r[4]; // return dest in $v0
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004c6c20u, strcpyStub);

        // 0x004c6d38: strlen(const char *s) -> returns length in $v0
        static auto strlenStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t sAddr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            const uint32_t sPhys = sAddr & 0x01FFFFFFu;
            uint32_t len = 0;
            if (sPhys < 0x02000000u) {
                const char *s = reinterpret_cast<const char *>(rdram + sPhys);
                while ((sPhys + len) < 0x02000000u && s[len] != '\0') {
                    len++;
                }
            }
            ctx->r[2] = _mm_cvtsi32_si128(static_cast<int32_t>(len));
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004c6d38u, strlenStub);

        // 0x004c6e70: strncat(char *dest, const char *src, size_t n) -> returns dest in $v0
        static auto strncatStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t destAddr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            const uint32_t srcAddr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
            const uint32_t n = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[6]));
            const uint32_t destPhys = destAddr & 0x01FFFFFFu;
            const uint32_t srcPhys = srcAddr & 0x01FFFFFFu;

            if (destPhys < 0x02000000u && srcPhys < 0x02000000u) {
                char *destStr = reinterpret_cast<char *>(rdram + destPhys);
                const char *srcStr = reinterpret_cast<const char *>(rdram + srcPhys);
                size_t destLen = 0;
                while ((destPhys + destLen) < 0x02000000u && destStr[destLen] != '\0') {
                    destLen++;
                }
                size_t i = 0;
                while (i < n && (destPhys + destLen + i) < 0x02000000u && (srcPhys + i) < 0x02000000u) {
                    destStr[destLen + i] = srcStr[i];
                    if (srcStr[i] == '\0') break;
                    i++;
                }
                if ((destPhys + destLen + i) < 0x02000000u) {
                    destStr[destLen + i] = '\0';
                }
            }
            ctx->r[2] = ctx->r[4]; // return dest in $v0
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004c6e70u, strncatStub);

        // 0x004c7020: strncmp(const char *s1, const char *s2, size_t n) -> returns int in $v0
        static auto strncmpStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t s1Addr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            const uint32_t s2Addr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
            const uint32_t n = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[6]));
            const uint32_t s1Phys = s1Addr & 0x01FFFFFFu;
            const uint32_t s2Phys = s2Addr & 0x01FFFFFFu;

            int diff = 0;
            if (n > 0 && s1Phys < 0x02000000u && s2Phys < 0x02000000u) {
                const uint8_t *u1 = rdram + s1Phys;
                const uint8_t *u2 = rdram + s2Phys;
                for (size_t i = 0; i < n && (s1Phys + i) < 0x02000000u && (s2Phys + i) < 0x02000000u; ++i) {
                    if (u1[i] != u2[i] || u1[i] == '\0') {
                        diff = static_cast<int>(u1[i]) - static_cast<int>(u2[i]);
                        break;
                    }
                }
            }
            ctx->r[2] = _mm_cvtsi32_si128(diff);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004c7020u, strncmpStub);

        // 0x004c7398: strrchr(const char *s, int c) -> returns pointer in $v0
        static auto strrchrStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t sAddr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            const char ch = static_cast<char>(_mm_cvtsi128_si32(ctx->r[5]));
            const uint32_t sPhys = sAddr & 0x01FFFFFFu;

            uint32_t result = 0;
            if (sPhys < 0x02000000u) {
                const char *s = reinterpret_cast<const char *>(rdram + sPhys);
                const size_t maxLen = 0x02000000u - sPhys;
                for (size_t i = 0; i < maxLen; ++i) {
                    if (s[i] == ch) {
                        result = sAddr + static_cast<uint32_t>(i);
                    }
                    if (s[i] == '\0') break;
                }
            }
            ctx->r[2] = _mm_cvtsi32_si128(static_cast<int32_t>(result));
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004c7398u, strrchrStub);

        // 0x004c73e8: strstr(const char *haystack, const char *needle) -> returns pointer in $v0
        static auto strstrStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t hAddr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            const uint32_t nAddr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
            const uint32_t hPhys = hAddr & 0x01FFFFFFu;
            const uint32_t nPhys = nAddr & 0x01FFFFFFu;

            uint32_t result = 0;
            if (hPhys < 0x02000000u && nPhys < 0x02000000u) {
                const char *h = reinterpret_cast<const char *>(rdram + hPhys);
                const char *n = reinterpret_cast<const char *>(rdram + nPhys);
                if (n[0] == '\0') {
                    result = hAddr;
                } else {
                    const size_t hMax = 0x02000000u - hPhys;
                    size_t nLen = 0;
                    while ((nPhys + nLen) < 0x02000000u && n[nLen] != '\0') nLen++;
                    if (nLen > 0) {
                        for (size_t i = 0; (i + nLen) <= hMax && h[i] != '\0'; ++i) {
                            if (std::memcmp(h + i, n, nLen) == 0) {
                                result = hAddr + static_cast<uint32_t>(i);
                                break;
                            }
                        }
                    }
                }
            }
            ctx->r[2] = _mm_cvtsi32_si128(static_cast<int32_t>(result));
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004c73e8u, strstrStub);

        // 0x004c6050: continuation / epilogue of FUN_004c6020
        static auto contStub4c6050 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t r4 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            const uint32_t r3 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[3]));
            const uint32_t sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));
            const uint32_t s1 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[17]));
            const uint32_t s0 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[16]));

            if (r4 != r3) { // bnel taken
                // delay slot: ld $16, 0($sp)
                if ((sp & 0x01FFFFFFu) + 8 <= 0x02000000u) {
                    uint64_t val = 0;
                    std::memcpy(&val, rdram + ((sp + 0) & 0x01FFFFFFu), 8);
                    ctx->r[16] = _mm_cvtsi64_si128(val);
                }
            } else { // bnel not taken
                uint32_t val3 = 0;
                if ((s1 & 0x01FFFFFFu) + 4 <= 0x02000000u) {
                    std::memcpy(&val3, rdram + (s1 & 0x01FFFFFFu), 4);
                }
                ctx->r[3] = _mm_cvtsi32_si128(static_cast<int32_t>(val3));
                if (val3 != 0) { // bnezl taken
                    if ((s0 & 0x01FFFFFFu) + 4 <= 0x02000000u) {
                        std::memcpy(rdram + (s0 & 0x01FFFFFFu), &val3, 4);
                    }
                }
                if ((sp & 0x01FFFFFFu) + 8 <= 0x02000000u) {
                    uint64_t val = 0;
                    std::memcpy(&val, rdram + ((sp + 0) & 0x01FFFFFFu), 8);
                    ctx->r[16] = _mm_cvtsi64_si128(val);
                }
            }
            // epilogue:
            // ld $17, 8($sp)
            // ld $ra, 16($sp)
            // addiu $sp, $sp, 0x20
            // jr $ra
            if ((sp & 0x01FFFFFFu) + 24 <= 0x02000000u) {
                uint64_t val17 = 0, valra = 0;
                std::memcpy(&val17, rdram + ((sp + 8) & 0x01FFFFFFu), 8);
                std::memcpy(&valra, rdram + ((sp + 16) & 0x01FFFFFFu), 8);
                ctx->r[17] = _mm_cvtsi64_si128(val17);
                ctx->r[31] = _mm_cvtsi64_si128(valra);
            }
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x20);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004c6050u, contStub4c6050);

        // 0x004c3704 / 0x004c3708: epilogue of FUN_004c3000 (malloc)
        static auto epilogue4c3704 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            // 0x4c3704: addiu $v0, $s0, 8
            const uint32_t s0Val = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[16]));
            ctx->r[2] = _mm_cvtsi32_si128(static_cast<int32_t>(s0Val + 8u));

            const uint32_t sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));
            if ((sp & 0x01FFFFFFu) + 0x30 <= 0x02000000u) {
                uint64_t s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0, ra = 0;
                std::memcpy(&s0, rdram + ((sp + 0x00) & 0x01FFFFFFu), 8);
                std::memcpy(&s1, rdram + ((sp + 0x08) & 0x01FFFFFFu), 8);
                std::memcpy(&s2, rdram + ((sp + 0x10) & 0x01FFFFFFu), 8);
                std::memcpy(&s3, rdram + ((sp + 0x18) & 0x01FFFFFFu), 8);
                std::memcpy(&s4, rdram + ((sp + 0x20) & 0x01FFFFFFu), 8);
                std::memcpy(&ra, rdram + ((sp + 0x28) & 0x01FFFFFFu), 8);

                ctx->r[16] = _mm_cvtsi64_si128(s0);
                ctx->r[17] = _mm_cvtsi64_si128(s1);
                ctx->r[18] = _mm_cvtsi64_si128(s2);
                ctx->r[19] = _mm_cvtsi64_si128(s3);
                ctx->r[20] = _mm_cvtsi64_si128(s4);
                ctx->r[31] = _mm_cvtsi64_si128(ra);
            }
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x30);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004c3704u, epilogue4c3704);
        runtime.replaceFunction(0x004c3708u, epilogue4c3704);

        // 0x004c2d44: epilogue of FUN_004c2d08
        static auto epilogue4c2d44 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            // move $v0, $s1
            ctx->r[2] = ctx->r[17];
            const uint32_t sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));
            if ((sp & 0x01FFFFFFu) + 0x20 <= 0x02000000u) {
                uint64_t s0 = 0, s1 = 0, ra = 0;
                std::memcpy(&s0, rdram + ((sp + 0x00) & 0x01FFFFFFu), 8);
                std::memcpy(&s1, rdram + ((sp + 0x08) & 0x01FFFFFFu), 8);
                std::memcpy(&ra, rdram + ((sp + 0x10) & 0x01FFFFFFu), 8);

                ctx->r[16] = _mm_cvtsi64_si128(s0);
                ctx->r[17] = _mm_cvtsi64_si128(s1);
                ctx->r[31] = _mm_cvtsi64_si128(ra);
            }
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x20);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004c2d44u, epilogue4c2d44);

        // 0x004c2b20: epilogue of FUN_004c2ad8
        static auto epilogue4c2b20 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            // 0x4c2b20: daddu $v0, $s1, $zero
            ctx->r[2] = ctx->r[17];

            const uint32_t sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));
            if ((sp & 0x01FFFFFFu) + 0x20 <= 0x02000000u) {
                uint64_t s0 = 0, s1 = 0, s2 = 0, ra = 0;
                std::memcpy(&s0, rdram + ((sp + 0x00) & 0x01FFFFFFu), 8);
                std::memcpy(&s1, rdram + ((sp + 0x08) & 0x01FFFFFFu), 8);
                std::memcpy(&s2, rdram + ((sp + 0x10) & 0x01FFFFFFu), 8);
                std::memcpy(&ra, rdram + ((sp + 0x18) & 0x01FFFFFFu), 8);

                ctx->r[16] = _mm_cvtsi64_si128(s0);
                ctx->r[17] = _mm_cvtsi64_si128(s1);
                ctx->r[18] = _mm_cvtsi64_si128(s2);
                ctx->r[31] = _mm_cvtsi64_si128(ra);
            }
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x20);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004c2b20u, epilogue4c2b20);

        // 0x004c2320: epilogue of FUN_004c21d8
        static auto epilogue4c2320 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            // 0x4c2320: addiu $v0, $zero, 1
            ctx->r[2] = _mm_cvtsi32_si128(1);

            const uint32_t sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));
            if ((sp & 0x01FFFFFFu) + 0x30 <= 0x02000000u) {
                uint64_t s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0, ra = 0;
                std::memcpy(&s0, rdram + ((sp + 0x00) & 0x01FFFFFFu), 8);
                std::memcpy(&s1, rdram + ((sp + 0x08) & 0x01FFFFFFu), 8);
                std::memcpy(&s2, rdram + ((sp + 0x10) & 0x01FFFFFFu), 8);
                std::memcpy(&s3, rdram + ((sp + 0x18) & 0x01FFFFFFu), 8);
                std::memcpy(&s4, rdram + ((sp + 0x20) & 0x01FFFFFFu), 8);
                std::memcpy(&ra, rdram + ((sp + 0x28) & 0x01FFFFFFu), 8);

                ctx->r[16] = _mm_cvtsi64_si128(s0);
                ctx->r[17] = _mm_cvtsi64_si128(s1);
                ctx->r[18] = _mm_cvtsi64_si128(s2);
                ctx->r[19] = _mm_cvtsi64_si128(s3);
                ctx->r[20] = _mm_cvtsi64_si128(s4);
                ctx->r[31] = _mm_cvtsi64_si128(ra);
            }
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x30);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004c2320u, epilogue4c2320);

        // 0x004c0328: epilogue of FUN_004c02e0
        static auto epilogue4c0328 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            // 0x4c0328: daddu $v0, $s1, $zero
            ctx->r[2] = ctx->r[17];

            const uint32_t sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));
            if ((sp & 0x01FFFFFFu) + 0x20 <= 0x02000000u) {
                uint64_t s0 = 0, s1 = 0, s2 = 0, ra = 0;
                std::memcpy(&s0, rdram + ((sp + 0x00) & 0x01FFFFFFu), 8);
                std::memcpy(&s1, rdram + ((sp + 0x08) & 0x01FFFFFFu), 8);
                std::memcpy(&s2, rdram + ((sp + 0x10) & 0x01FFFFFFu), 8);
                std::memcpy(&ra, rdram + ((sp + 0x18) & 0x01FFFFFFu), 8);

                ctx->r[16] = _mm_cvtsi64_si128(s0);
                ctx->r[17] = _mm_cvtsi64_si128(s1);
                ctx->r[18] = _mm_cvtsi64_si128(s2);
                ctx->r[31] = _mm_cvtsi64_si128(ra);
            }
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x20);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004c0328u, epilogue4c0328);

        // 0x004c1db0: epilogue of FUN_004c1d68
        static auto epilogue4c1db0 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));
            if ((sp & 0x01FFFFFFu) + 0x60 <= 0x02000000u) {
                uint64_t ra = 0;
                std::memcpy(&ra, rdram + ((sp + 0x00) & 0x01FFFFFFu), 8);
                ctx->r[31] = _mm_cvtsi64_si128(ra);
            }
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x60);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004c1db0u, epilogue4c1db0);

        // 0x004c44e8: epilogue of FUN_004c43b8
        static auto epilogue4c44e8 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            // 0x4c44e8: daddu $v0, $s4, $zero
            ctx->r[2] = ctx->r[20];

            const uint32_t sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));
            if ((sp & 0x01FFFFFFu) + 0x40 <= 0x02000000u) {
                uint64_t s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0, s5 = 0, ra = 0;
                std::memcpy(&s0, rdram + ((sp + 0x00) & 0x01FFFFFFu), 8);
                std::memcpy(&s1, rdram + ((sp + 0x08) & 0x01FFFFFFu), 8);
                std::memcpy(&s2, rdram + ((sp + 0x10) & 0x01FFFFFFu), 8);
                std::memcpy(&s3, rdram + ((sp + 0x18) & 0x01FFFFFFu), 8);
                std::memcpy(&s4, rdram + ((sp + 0x20) & 0x01FFFFFFu), 8);
                std::memcpy(&s5, rdram + ((sp + 0x28) & 0x01FFFFFFu), 8);
                std::memcpy(&ra, rdram + ((sp + 0x30) & 0x01FFFFFFu), 8);

                ctx->r[16] = _mm_cvtsi64_si128(s0);
                ctx->r[17] = _mm_cvtsi64_si128(s1);
                ctx->r[18] = _mm_cvtsi64_si128(s2);
                ctx->r[19] = _mm_cvtsi64_si128(s3);
                ctx->r[20] = _mm_cvtsi64_si128(s4);
                ctx->r[21] = _mm_cvtsi64_si128(s5);
                ctx->r[31] = _mm_cvtsi64_si128(ra);
            }
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x40);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004c44e8u, epilogue4c44e8);

        // 0x004c4cfc: epilogue of FUN_004c4c98
        static auto epilogue4c4cfc = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));
            if ((sp & 0x01FFFFFFu) + 0x70 <= 0x02000000u) {
                uint64_t ra = 0;
                std::memcpy(&ra, rdram + ((sp + 0x00) & 0x01FFFFFFu), 8);
                ctx->r[31] = _mm_cvtsi64_si128(ra);
            }
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x70);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004c4cfcu, epilogue4c4cfc);

        // 0x004c5820: epilogue of FUN_004c57d8
        static auto epilogue4c5820 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            // 0x4c5820: daddu $v0, $s1, $zero
            ctx->r[2] = ctx->r[17];

            const uint32_t sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));
            if ((sp & 0x01FFFFFFu) + 0x20 <= 0x02000000u) {
                uint64_t s0 = 0, s1 = 0, s2 = 0, ra = 0;
                std::memcpy(&s0, rdram + ((sp + 0x00) & 0x01FFFFFFu), 8);
                std::memcpy(&s1, rdram + ((sp + 0x08) & 0x01FFFFFFu), 8);
                std::memcpy(&s2, rdram + ((sp + 0x10) & 0x01FFFFFFu), 8);
                std::memcpy(&ra, rdram + ((sp + 0x18) & 0x01FFFFFFu), 8);

                ctx->r[16] = _mm_cvtsi64_si128(s0);
                ctx->r[17] = _mm_cvtsi64_si128(s1);
                ctx->r[18] = _mm_cvtsi64_si128(s2);
                ctx->r[31] = _mm_cvtsi64_si128(ra);
            }
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x20);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004c5820u, epilogue4c5820);

        // 0x004cb470 / 0x004cb4c8: epilogue of FUN_004cb430 / FUN_004cb488
        static auto epilogue4cb470 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));
            if ((sp & 0x01FFFFFFu) + 0x70 <= 0x02000000u) {
                uint32_t v1 = 0;
                std::memcpy(&v1, rdram + ((sp + 0x00) & 0x01FFFFFFu), 4);
                if ((v1 & 0x01FFFFFFu) < 0x02000000u) {
                    rdram[v1 & 0x01FFFFFFu] = '\0';
                }
                uint64_t ra = 0;
                std::memcpy(&ra, rdram + ((sp + 0x60) & 0x01FFFFFFu), 8);
                ctx->r[31] = _mm_cvtsi64_si128(ra);
            }
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x70);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004cb470u, epilogue4cb470);
        runtime.replaceFunction(0x004cb4c8u, epilogue4cb470);

        // 0x004cfe38 / 0x004cfe70 / etc: ld $ra, 0($sp); jr $ra; addiu $sp, $sp, 0x10
        static auto epilogueCdReturn10 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));
            if ((sp & 0x01FFFFFFu) + 0x10 <= 0x02000000u) {
                uint64_t ra = 0;
                std::memcpy(&ra, rdram + ((sp + 0x00) & 0x01FFFFFFu), 8);
                ctx->r[31] = _mm_cvtsi64_si128(ra);
            }
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x10);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004cfe38u, epilogueCdReturn10);
        runtime.replaceFunction(0x004cfe70u, epilogueCdReturn10);
        runtime.replaceFunction(0x004cfea0u, epilogueCdReturn10);
        runtime.replaceFunction(0x004cfed0u, epilogueCdReturn10);
        runtime.replaceFunction(0x004cff0cu, epilogueCdReturn10);

        // Native libm stubs
        static auto powfStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->f[0] = std::pow(ctx->f[12], ctx->f[13]);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004ce518u, powfStub);
        runtime.replaceFunction(0x004cc530u, powfStub);

        static auto sinfStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->f[0] = std::sin(ctx->f[12]);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004ce500u, sinfStub);
        runtime.replaceFunction(0x004cc250u, sinfStub);

        static auto cosfStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->f[0] = std::cos(ctx->f[12]);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004ce4e8u, cosfStub);
        runtime.replaceFunction(0x004cbff8u, cosfStub);

        static auto atan2fStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->f[0] = std::atan2(ctx->f[12], ctx->f[13]);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004ce4d0u, atan2fStub);
        runtime.replaceFunction(0x004cbe18u, atan2fStub);

        static auto asinfStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->f[0] = std::asin(ctx->f[12]);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004ce4b8u, asinfStub);
        runtime.replaceFunction(0x004cba78u, asinfStub);

        static auto atanfStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->f[0] = std::atan(ctx->f[12]);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004ce4a0u, atanfStub);
        runtime.replaceFunction(0x004cb650u, atanfStub);

        // 0x004c3728: FUN_004c3728
        static auto stub4c3728 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t a1 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
            const uint32_t a2 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[6]));
            const uint32_t a3 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[7]));
            const uint32_t sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));

            uint32_t v1 = sp - 0x10;
            if (a1 != 0) {
                v1 = a1;
            }

            int32_t v0 = 0;
            if (a2 != 0) {
                if (a3 == 0) {
                    v0 = -1;
                } else {
                    const uint32_t a2Phys = a2 & 0x01FFFFFFu;
                    uint8_t byteVal = 0;
                    if (a2Phys < 0x02000000u) {
                        byteVal = rdram[a2Phys];
                    }
                    const uint32_t v1Phys = v1 & 0x01FFFFFFu;
                    if (v1Phys + 4 <= 0x02000000u) {
                        uint32_t wordVal = byteVal;
                        std::memcpy(rdram + v1Phys, &wordVal, 4);
                    }
                    v0 = (byteVal > 0) ? 1 : 0;
                }
            }

            ctx->r[2] = _mm_cvtsi32_si128(v0);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004c3728u, stub4c3728);

        // 0x004c3760: memchr(const void *s, int c, size_t n) -> returns pointer in $v0
        static auto memchrStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t sAddr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            const uint8_t ch = static_cast<uint8_t>(_mm_cvtsi128_si32(ctx->r[5]) & 0xFF);
            const uint32_t n = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[6]));
            const uint32_t sPhys = sAddr & 0x01FFFFFFu;

            uint32_t result = 0;
            if (n > 0 && sPhys < 0x02000000u) {
                const size_t searchBytes = std::min(static_cast<size_t>(n), static_cast<size_t>(0x02000000u - sPhys));
                const uint8_t *ptr = static_cast<const uint8_t *>(std::memchr(rdram + sPhys, ch, searchBytes));
                if (ptr) {
                    result = sAddr + static_cast<uint32_t>(ptr - (rdram + sPhys));
                }
            }
            ctx->r[2] = _mm_cvtsi32_si128(static_cast<int32_t>(result));
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004c3760u, memchrStub);

        // 0x004c3840: memcmp(const void *s1, const void *s2, size_t n) -> returns diff in $v0
        static auto memcmpStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t s1Addr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            const uint32_t s2Addr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
            const uint32_t n = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[6]));
            const uint32_t s1Phys = s1Addr & 0x01FFFFFFu;
            const uint32_t s2Phys = s2Addr & 0x01FFFFFFu;

            int diff = 0;
            if (n > 0 && s1Phys < 0x02000000u && s2Phys < 0x02000000u) {
                const size_t cmpBytes = std::min(static_cast<size_t>(n),
                                                  std::min(static_cast<size_t>(0x02000000u - s1Phys),
                                                           static_cast<size_t>(0x02000000u - s2Phys)));
                diff = std::memcmp(rdram + s1Phys, rdram + s2Phys, cmpBytes);
            }
            ctx->r[2] = _mm_cvtsi32_si128(diff);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004c3840u, memcmpStub);

        // 0x004c38d8: memcpy(void *dest, const void *src, size_t n) -> returns dest in $v0
        static auto memcpyStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t destAddr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            const uint32_t srcAddr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
            const uint32_t n = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[6]));
            const uint32_t destPhys = destAddr & 0x01FFFFFFu;
            const uint32_t srcPhys = srcAddr & 0x01FFFFFFu;

            if (n > 0 && destPhys < 0x02000000u && srcPhys < 0x02000000u) {
                const size_t copyBytes = std::min(static_cast<size_t>(n),
                                                   std::min(static_cast<size_t>(0x02000000u - destPhys),
                                                            static_cast<size_t>(0x02000000u - srcPhys)));
                std::memcpy(rdram + destPhys, rdram + srcPhys, copyBytes);
            }
            // 2026-10-10 fix: el loop que arma la tabla del burst vive en p10: jal memcpy en
            // 0x74B8F0 -> ret 0x74B8F8, con `while (fp < *(sp+0x28))` (fp=$r30, limit=*(sp+0x28)).
            // Leer el bound y el máximo destino para ver por qué se corta en 0xCF0000.
            if (destPhys >= 0x00C50000u && destPhys < 0x00DB0000u) {
                if (destPhys > g_burstMaxDst) g_burstMaxDst = destPhys;
                const uint32_t ra = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
                if (ra == 0x74B8F8u) {
                    const uint32_t sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));
                    uint32_t limit = 0u;
                    if (sp + 0x30u <= 0x02000000u)
                        std::memcpy(&limit, rdram + ((sp + 0x28u) & 0x01FFFFFFu), 4u);
                    static int s_nb = 0;
                    ++s_nb;
                    if (s_nb <= 24 || (s_nb % 500) == 0 || destPhys >= 0x00CE0000u) {
                        RUNTIME_LOG("[burst-loop] i=" << s_nb << " dst=0x" << std::hex << destPhys
                                    << " src=0x" << srcPhys
                                    << " n=0x" << n << " fp=0x" << static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[30]))
                                    << " limit=0x" << limit << std::dec);
                    }
                }
            }
            ctx->r[2] = ctx->r[4]; // return dest in $v0
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004c38d8u, memcpyStub);

        // 0x004c3988: memmove(void *dest, const void *src, size_t n) -> returns dest in $v0
        static auto memmoveStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t destAddr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            const uint32_t srcAddr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
            const uint32_t n = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[6]));
            const uint32_t destPhys = destAddr & 0x01FFFFFFu;
            const uint32_t srcPhys = srcAddr & 0x01FFFFFFu;

            if (n > 0 && destPhys < 0x02000000u && srcPhys < 0x02000000u) {
                const size_t copyBytes = std::min(static_cast<size_t>(n),
                                                   std::min(static_cast<size_t>(0x02000000u - destPhys),
                                                            static_cast<size_t>(0x02000000u - srcPhys)));
                std::memmove(rdram + destPhys, rdram + srcPhys, copyBytes);
            }
            ctx->r[2] = ctx->r[4]; // return dest in $v0
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004c3988u, memmoveStub);

        // 0x004c3a90: memset(void *dest, int c, size_t n) -> returns dest in $v0
        static auto memsetStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t destAddr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            const uint8_t val = static_cast<uint8_t>(_mm_cvtsi128_si32(ctx->r[5]) & 0xFF);
            const uint32_t n = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[6]));
            const uint32_t destPhys = destAddr & 0x01FFFFFFu;

            if (n > 0 && destPhys < 0x02000000u) {
                const size_t fillBytes = std::min(static_cast<size_t>(n), static_cast<size_t>(0x02000000u - destPhys));
                std::memset(rdram + destPhys, val, fillBytes);
            }
            ctx->r[2] = ctx->r[4]; // return dest in $v0
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004c3a90u, memsetStub);

        // 0x004c5f98: __isinf(double in $a0) -> returns int in $v0
        static auto isinfStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            const uint64_t a0 = _mm_cvtsi128_si64(ctx->r[4]);
            const uint32_t low = static_cast<uint32_t>(a0);
            const uint32_t high = static_cast<uint32_t>(a0 >> 32) & 0x7FFFFFFFu;
            const int32_t res = (high == 0x7FF00000u && low == 0) ? 1 : 0;
            ctx->r[2] = _mm_cvtsi32_si128(res);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004c5f98u, isinfStub);

        // 0x004c5fe0: __isnan(double in $a0) -> returns int in $v0
        static auto isnanStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            const uint64_t a0 = _mm_cvtsi128_si64(ctx->r[4]);
            const uint32_t low = static_cast<uint32_t>(a0);
            const uint32_t high = static_cast<uint32_t>(a0 >> 32) & 0x7FFFFFFFu;
            const int32_t res = (high > 0x7FF00000u || (high == 0x7FF00000u && low != 0)) ? 1 : 0;
            ctx->r[2] = _mm_cvtsi32_si128(res);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004c5fe0u, isnanStub);

        // 0x004ca400: __dtoa wrapper for vfprintf
        static auto dtoaWrapperStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const int32_t prec = static_cast<int32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            const uint64_t dBits = _mm_cvtsi128_si64(ctx->r[5]);
            double val;
            std::memcpy(&val, &dBits, sizeof(val));

            const uint32_t signBufAddr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[8]));
            const uint32_t decptAddr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[9]));
            const char fmtChar = static_cast<char>(_mm_cvtsi128_si32(ctx->r[10]) & 0xFF);
            const uint32_t lenAddr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[11]));
            const uint32_t sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));

            bool isNeg = std::signbit(val);
            if (isNeg) {
                val = -val;
            }

            // Write sign to signBuf
            if ((signBufAddr & 0x01FFFFFFu) + 2 <= 0x02000000u) {
                rdram[(signBufAddr & 0x01FFFFFFu) + 0] = isNeg ? '-' : '\0';
                rdram[(signBufAddr & 0x01FFFFFFu) + 1] = '\0';
            }

            // Format number with std::snprintf
            char buf[128];
            int p = (prec < 0) ? 6 : ((prec > 30) ? 30 : prec);
            if (fmtChar == 'e' || fmtChar == 'E') {
                std::snprintf(buf, sizeof(buf), "%.*e", p, val);
            } else {
                std::snprintf(buf, sizeof(buf), "%.*f", p, val);
            }

            // Write digits to stack area (sp - 0x40)
            const uint32_t digitsGuestAddr = (sp >= 0x40) ? (sp - 0x40) : 0x872800u;
            char *digitsDest = reinterpret_cast<char *>(rdram + (digitsGuestAddr & 0x01FFFFFFu));

            int decpt = 0;
            size_t outIdx = 0;
            bool seenDot = false;
            for (size_t i = 0; buf[i] != '\0' && buf[i] != 'e' && buf[i] != 'E' && outIdx < 60; ++i) {
                if (buf[i] == '.') {
                    seenDot = true;
                    decpt = static_cast<int>(outIdx);
                } else if (buf[i] >= '0' && buf[i] <= '9') {
                    digitsDest[outIdx++] = buf[i];
                }
            }
            if (!seenDot) {
                decpt = static_cast<int>(outIdx);
            }
            digitsDest[outIdx] = '\0';

            // Store *decpt
            if ((decptAddr & 0x01FFFFFFu) + 4 <= 0x02000000u) {
                std::memcpy(rdram + (decptAddr & 0x01FFFFFFu), &decpt, 4);
            }

            // Store *len
            int32_t len = static_cast<int32_t>(outIdx);
            if ((lenAddr & 0x01FFFFFFu) + 4 <= 0x02000000u) {
                std::memcpy(rdram + (lenAddr & 0x01FFFFFFu), &len, 4);
            }

            // Return pointer to digits in $v0
            ctx->r[2] = _mm_cvtsi32_si128(static_cast<int32_t>(digitsGuestAddr));
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004ca400u, dtoaWrapperStub);

        // 0x004c65e4: epilogue of FUN_004c6568 (vsprintf)
        static auto epilogue4c65e4 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));
            if ((sp & 0x01FFFFFFu) + 0xc0 <= 0x02000000u) {
                uint32_t r3 = 0;
                std::memcpy(&r3, rdram + ((sp + 0x00) & 0x01FFFFFFu), 4);
                if ((r3 & 0x01FFFFFFu) < 0x02000000u) {
                    rdram[r3 & 0x01FFFFFFu] = '\0';
                }
                uint64_t ra = 0;
                std::memcpy(&ra, rdram + ((sp + 0x60) & 0x01FFFFFFu), 8);
                ctx->r[31] = _mm_cvtsi64_si128(ra);
            }
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0xc0);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004c65e4u, epilogue4c65e4);

        // 0x004e9aa8: scePadInit(mode in $a0) -> returns int in $v0
        static auto padInitStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            const uint32_t mode = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            RUNTIME_LOG("[game_overrides] scePadInit (0x4e9aa8) mode=" << mode);

            ps2_stubs::scePadInit(rdram, ctx, runtime);

            // 1. Mark pad initialized in game globals:
            const uint32_t one = 1;
            std::memcpy(rdram + (0x008755f0u & 0x01FFFFFFu), &one, 4);

            // 2. Set up pad RPC clients at 0xa31940 and 0xa31968
            const uint32_t client0 = 0x00a31940u;
            const uint32_t client1 = 0x00a31968u;
            const uint32_t dummyServer0 = 0x00a29800u;
            const uint32_t dummyServer1 = 0x00a29840u;
            const uint32_t dummyBuf = 0x00a29880u;

            // Server 0
            std::memset(rdram + (dummyServer0 & 0x01FFFFFFu), 0, 64);
            const uint32_t rpcId0 = 0x80000100u;
            std::memcpy(rdram + (dummyServer0 & 0x01FFFFFFu), &rpcId0, 4);
            std::memcpy(rdram + ((dummyServer0 + 28) & 0x01FFFFFFu), &client0, 4);

            // Server 1
            std::memset(rdram + (dummyServer1 & 0x01FFFFFFu), 0, 64);
            const uint32_t rpcId1 = 0x80000101u;
            std::memcpy(rdram + (dummyServer1 & 0x01FFFFFFu), &rpcId1, 4);
            std::memcpy(rdram + ((dummyServer1 + 28) & 0x01FFFFFFu), &client1, 4);

            // Client descriptors
            std::memcpy(rdram + ((client0 + 0x14u) & 0x01FFFFFFu), &dummyBuf, 4);
            std::memcpy(rdram + ((client0 + 0x18u) & 0x01FFFFFFu), &dummyBuf, 4);
            std::memcpy(rdram + ((client0 + 0x24u) & 0x01FFFFFFu), &dummyServer0, 4);
            std::memcpy(rdram + ((client0 + 0x4cu) & 0x01FFFFFFu), &dummyServer1, 4);

            std::memcpy(rdram + ((client1 + 0x14u) & 0x01FFFFFFu), &dummyBuf, 4);
            std::memcpy(rdram + ((client1 + 0x18u) & 0x01FFFFFFu), &dummyBuf, 4);
            std::memcpy(rdram + ((client1 + 0x24u) & 0x01FFFFFFu), &dummyServer1, 4);

            // 3. Set libpad version (0x0401)
            const uint32_t version = 0x0401u;
            std::memcpy(rdram + (0x00a31b8cu & 0x01FFFFFFu), &version, 4);

            // 4. Initialize port data structures (clear 0xa31990)
            std::memset(rdram + (0x00a31990u & 0x01FFFFFFu), 0, 0x100);

            ctx->r[2] = _mm_cvtsi32_si128(1); // return 1 (success)
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004e9aa8u, padInitStub);

        // 0x004e9b14: FUN_004e9aa8 continuation stub
        static auto returnStub4e9b14 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            const uint32_t sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));
            if ((sp & 0x01FFFFFFu) + 0x40 <= 0x02000000u) {
                uint64_t raVal = 0, s2Val = 0, s1Val = 0, s0Val = 0;
                std::memcpy(&raVal, rdram + ((sp + 0x30) & 0x01FFFFFFu), 8);
                std::memcpy(&s2Val, rdram + ((sp + 0x20) & 0x01FFFFFFu), 8);
                std::memcpy(&s1Val, rdram + ((sp + 0x10) & 0x01FFFFFFu), 8);
                std::memcpy(&s0Val, rdram + ((sp + 0x00) & 0x01FFFFFFu), 8);
                ctx->r[31] = _mm_cvtsi64_si128(raVal);
                ctx->r[18] = _mm_cvtsi64_si128(s2Val);
                ctx->r[17] = _mm_cvtsi64_si128(s1Val);
                ctx->r[16] = _mm_cvtsi64_si128(s0Val);
            }
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x40);
            ctx->r[2] = _mm_cvtsi32_si128(1); // return 1
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004e9b14u, returnStub4e9b14);

        // =========================================================================
        // Libpad Stubs for Sword of Etheria (SLES_537.68)
        // =========================================================================

        // 0x004e9eb0: scePadPortOpen(port in $a0, slot in $a1, void *data in $a2) -> returns 1
        static uint32_t s_registeredPadDmaAddr[2][4] = {{0}};
        static auto padPortOpenStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            const uint32_t port = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            const uint32_t slotArg = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
            const uint32_t slot = (slotArg < 4) ? slotArg : 0u;
            const uint32_t dataAddr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[6]));
            RUNTIME_LOG("[game_overrides] scePadPortOpen (0x4e9eb0) port=" << port << " slot=" << slot << " data=0x" << std::hex << dataAddr << std::dec);

            ctx->r[5] = _mm_cvtsi32_si128(static_cast<int32_t>(slot));
            ps2_stubs::scePadPortOpen(rdram, ctx, runtime);

            if (port < 2 && slot < 4) {
                s_registeredPadDmaAddr[port][slot] = dataAddr;
            }

            // Set up internal libpad EE state struct at 0x00a31990
            const uint32_t portOffset = 0x00a31990u + port * 0x70u + slot * 0x1cu;
            const uint32_t effectiveBuf = (dataAddr != 0) ? dataAddr : (0x00a31c00u + port * 0x100u + slot * 0x40u);
            if ((portOffset & 0x01FFFFFFu) + 0x20 <= 0x02000000u) {
                const uint32_t one = 1;
                const uint32_t stableState = 6; // PAD_STATE_STABLE
                std::memcpy(rdram + (portOffset & 0x01FFFFFFu) + 0x00, &effectiveBuf, 4);
                std::memcpy(rdram + (portOffset & 0x01FFFFFFu) + 0x04, &effectiveBuf, 4);
                std::memcpy(rdram + (portOffset & 0x01FFFFFFu) + 0x08, &one, 4);
                std::memcpy(rdram + (portOffset & 0x01FFFFFFu) + 0x0c, &stableState, 4);
                std::memcpy(rdram + (portOffset & 0x01FFFFFFu) + 0x10, &one, 4); // open flag = 1
                std::memcpy(rdram + (portOffset & 0x01FFFFFFu) + 0x14, &effectiveBuf, 4);
            }

            // Also ensure slot 0 at base is initialized
            const uint32_t slot0Offset = 0x00a31990u + port * 0x70u;
            if ((slot0Offset & 0x01FFFFFFu) + 0x20 <= 0x02000000u) {
                const uint32_t one = 1;
                const uint32_t stableState = 6;
                std::memcpy(rdram + (slot0Offset & 0x01FFFFFFu) + 0x00, &effectiveBuf, 4);
                std::memcpy(rdram + (slot0Offset & 0x01FFFFFFu) + 0x04, &effectiveBuf, 4);
                std::memcpy(rdram + (slot0Offset & 0x01FFFFFFu) + 0x08, &one, 4);
                std::memcpy(rdram + (slot0Offset & 0x01FFFFFFu) + 0x0c, &stableState, 4);
                std::memcpy(rdram + (slot0Offset & 0x01FFFFFFu) + 0x10, &one, 4);
                std::memcpy(rdram + (slot0Offset & 0x01FFFFFFu) + 0x14, &effectiveBuf, 4);
            }

            // Initialize the buffer as a connected DualShock 2 (mode 0x79)
            if (effectiveBuf != 0 && (effectiveBuf & 0x01FFFFFFu) + 32 <= 0x02000000u) {
                uint8_t *buf = rdram + (effectiveBuf & 0x01FFFFFFu);
                std::memset(buf, 0, 32);
                buf[0] = 0x00; // ok
                buf[1] = 0x79; // DualShock 2 (pressure mode)
                buf[2] = 0xFF; // no buttons pressed
                buf[3] = 0xFF;
                buf[4] = 0x80; // stick center
                buf[5] = 0x80;
                buf[6] = 0x80;
                buf[7] = 0x80;
            }

            ctx->r[2] = _mm_cvtsi32_si128(1); // return 1
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004e9eb0u, padPortOpenStub);

        // 0x004ea08c: Return stub for 0x004e9eb0 epilogue (jr $ra; addiu $sp, $sp, 0xb0)
        static auto returnStub4ea08c = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0xb0);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004ea08cu, returnStub4ea08c);

        // 0x004ea098: scePadPortClose(port in $a0, slot in $a1) -> returns 1
        static auto padPortCloseStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            const uint32_t port = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            const uint32_t slot = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
            RUNTIME_LOG("[game_overrides] scePadPortClose (0x4ea098) port=" << port << " slot=" << slot);
            ps2_stubs::scePadPortClose(rdram, ctx, runtime);
            const uint32_t portOffset = 0x00a31990u + port * 0x70u + slot * 0x1cu;
            if ((portOffset & 0x01FFFFFFu) + 0x20 <= 0x02000000u) {
                const uint32_t zero = 0;
                std::memcpy(rdram + (portOffset & 0x01FFFFFFu) + 0x10, &zero, 4); // open flag = 0
            }
            if (port < 2 && slot < 4) {
                s_registeredPadDmaAddr[port][slot] = 0;
            }
            ctx->r[2] = _mm_cvtsi32_si128(1);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004ea098u, padPortCloseStub);

        // 0x004ea200: scePadRead(port in $a0, slot in $a1, unsigned char *rdata in $a2) -> returns bytesRead (32)
        static uint32_t s_padReadCallCount = 0;
        static auto padReadStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            const uint32_t port = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            const uint32_t slotArg = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
            const uint32_t slot = (slotArg < 4) ? slotArg : 0u;
            ctx->r[5] = _mm_cvtsi32_si128(static_cast<int32_t>(slot));
            uint32_t rdata = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[6]));
            const uint32_t dmaAddr = (port < 2 && slot < 4) ? s_registeredPadDmaAddr[port][slot] : 0;
            if (rdata == 0 && dmaAddr != 0) {
                rdata = dmaAddr;
                ctx->r[6] = _mm_cvtsi32_si128(static_cast<int32_t>(rdata));
            }

            ps2_stubs::scePadRead(rdram, ctx, runtime);
            int32_t bytesRead = _mm_cvtsi128_si32(ctx->r[2]);
            if (bytesRead <= 0) {
                bytesRead = 32;
            }

            s_padReadCallCount++;
            if (s_padReadCallCount < 10 || (s_padReadCallCount % 300) == 0) {
                RUNTIME_LOG("[game_overrides] scePadRead call #" << s_padReadCallCount << " port=" << port);
            }
            // Populate/sync both rdata and registered DMA buffer
            auto syncBuffer = [&](uint32_t addr) {
                if (addr != 0 && (addr & 0x01FFFFFFu) + 32 <= 0x02000000u) {
                    uint8_t *buf = rdram + (addr & 0x01FFFFFFu);
                    if (buf[1] == 0) buf[1] = 0x79; // DualShock 2
                    if (buf[2] == 0 && buf[3] == 0) {
                        buf[2] = 0xFF; // released by default
                        buf[3] = 0xFF;
                    }
                    if (buf[4] == 0 && buf[5] == 0 && buf[6] == 0 && buf[7] == 0) {
                        buf[4] = 0x80; buf[5] = 0x80; buf[6] = 0x80; buf[7] = 0x80;
                    }
                    // Auto-press START / CROSS periodically after frame 180 to dismiss intro & title
                    if (port == 0 && s_padReadCallCount > 180 && (s_padReadCallCount % 60) < 15) {
                        buf[2] &= 0xF7; // Press START (bit 3)
                        buf[3] &= 0xBF; // Press CROSS (bit 6)
                    }
                }
            };
            syncBuffer(rdata);
            if (dmaAddr != 0 && dmaAddr != rdata) {
                syncBuffer(dmaAddr);
            }

            ctx->r[2] = _mm_cvtsi32_si128(bytesRead);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004ea200u, padReadStub);

        // 0x004ea2b8: scePadSetMainMode(port, slot, mode, lock)
        static auto padSetMainModeStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            ps2_stubs::scePadSetMainMode(rdram, ctx, runtime);
            ctx->r[2] = _mm_cvtsi32_si128(1);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004ea2b8u, padSetMainModeStub);

        // 0x004ea460: scePadGetState(port, slot) -> returns state in $v0 (6 = STABLE)
        static auto padGetStateStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            ps2_stubs::scePadGetState(rdram, ctx, runtime);
            int32_t state = _mm_cvtsi128_si32(ctx->r[2]);
            if (state == 0) {
                state = 6; // PAD_STATE_STABLE
                ctx->r[2] = _mm_cvtsi32_si128(state);
            }
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004ea460u, padGetStateStub);

        // 0x004ea4e8: scePadGetReqState(port, slot) -> returns req state in $v0 (0 = complete)
        static auto padGetReqStateStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->r[2] = _mm_setzero_si128(); // 0 = complete
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004ea4e8u, padGetReqStateStub);

        // 0x004ea728: scePadInfoMode(port, slot, term, offs) -> returns mode/count in $v0
        static auto padInfoModeStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            ps2_stubs::scePadInfoMode(rdram, ctx, runtime);
            int32_t res = _mm_cvtsi128_si32(ctx->r[2]);
            if (res == 0) {
                res = 7; // DualShock 2
                ctx->r[2] = _mm_cvtsi32_si128(res);
            }
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004ea728u, padInfoModeStub);

        // 0x004ea860: scePadSetMainModeRpc(port, slot, actNo, term)
        static auto padInfoActStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            ps2_stubs::scePadInfoAct(rdram, ctx, runtime);
            ctx->r[2] = _mm_cvtsi32_si128(1);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004ea860u, padInfoActStub);

        // 0x004ea918: scePadInfoComb(port, slot, type, id)
        static auto padInfoCombStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            ps2_stubs::scePadInfoComb(rdram, ctx, runtime);
            ctx->r[2] = _mm_setzero_si128();
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004ea918u, padInfoCombStub);

        // 0x004ea9d8: scePadSetActDirectRpc
        static auto padSetActDirectRpcStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->r[2] = _mm_cvtsi32_si128(1);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004ea9d8u, padSetActDirectRpcStub);

        // 0x004eac18: scePadEnterPressMode(port, slot)
        static auto padEnterPressModeStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            ps2_stubs::scePadEnterPressMode(rdram, ctx, runtime);
            ctx->r[2] = _mm_cvtsi32_si128(1);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004eac18u, padEnterPressModeStub);

        // 0x004eadf8: scePadEnd() -> returns 1
        static auto padEndStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            ps2_stubs::scePadEnd(rdram, ctx, runtime);
            ctx->r[2] = _mm_cvtsi32_si128(1);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004eadf8u, padEndStub);

        // 0x004eae60: pad RPC init helper
        static auto padRpcInitStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->r[2] = _mm_cvtsi32_si128(1);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004eae60u, padRpcInitStub);

        // 0x004eaec8: scePadGetModVersion()
        static auto padGetModVersionStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->r[2] = _mm_cvtsi32_si128(0x0401);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004eaec8u, padGetModVersionStub);

        // 0x004d0cd0: FUN_004d0cd0 -> returns 0x00873c90 (GS context pointer)
        static auto stub4d0cd0 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->r[2] = _mm_cvtsi32_si128(0x00873c90u);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d0cd0u, stub4d0cd0);

        // 2026-10-11: ELIMINADOS los dos loops que reemplazaban masivamente slots de
        // 0x4eb8d0..0x6cba48 y 0x6cba48..0x9cbd34 por FUN_004eb8d0 / FUN_006cba48.
        // Eran un workaround de cuando esos End del CSV estaban INFLADOS (3.2 MB) y
        // las megas inlineaban todo. Ahora los End estan corregidos (megas de KB), el
        // registro por funcion es correcto, y esos loops pisaban TODAS las funciones
        // del juego con los 2 stubs diminutos -> paralisis + flood de
        // "No exact recompiled function".

        // TRIAGE pantalla negra (2026-10-03, temporal solo para clasificar).
        // VA DESPUÉS de los redirects fullGame/fullRender: 0x6e4d98 cae dentro
        // del rango 0x6cba48..0x9cbd34 y el redirect lo pisa si se registra antes
        // (verificado: sin [triage] en coldboot-triage.log, count 18884->18885).
        // v2 2026-10-04: el loop sale solo si 0x638==0x63C Y byte gp+0x630==0
        // (ver análisis en game/6e4d98-spinwait-triage.md). v1 solo igualaba
        // contadores y seguía girando. v2 limpia también el flag y salta a
        // 0x6e4dac: o avanza (nuevos gif/VRAM) o crashea con pc revelador.
        static auto triage6e4d98 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t gp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[28]));
            uint32_t a = 0, b = 0;
            uint8_t flag = 0, flag630 = 0, flag640 = 0;
            std::memcpy(&a, rdram + ((gp + 0x638u) & 0x01FFFFFFu), 4);
            std::memcpy(&b, rdram + ((gp + 0x63Cu) & 0x01FFFFFFu), 4);
            std::memcpy(&flag, rdram + ((gp + 0x5D0u) & 0x01FFFFFFu), 1);
            std::memcpy(&flag630, rdram + ((gp + 0x630u) & 0x01FFFFFFu), 1);
            std::memcpy(&flag640, rdram + ((gp + 0x640u) & 0x01FFFFFFu), 1);
            static int triageCount = 0;
            if (++triageCount % 200 == 1) {
                RUNTIME_LOG("[triage] 0x6e4d98 counters a=" << a << " b=" << b << " flag5D0=" << (unsigned)flag << " flag630=" << (unsigned)flag630 << " flag640=" << (unsigned)flag640);
            }
            // 2026-10-05 (visual-primero): en las 3 primeras visitas,
            // registrar ra/sp para nombrar la función que espera.
            // 2026-10-06s: ra en CADA visita (throttle): la distribución
            // dice la estructura del ciclo (0x6e4cdc = vía worker-jal,
            // 0x6e4dd0 = vía cb, OTRO = nuevo caller).
            if (triageCount <= 3 || triageCount % 200 == 1) {
                const uint32_t ra = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
                const uint32_t sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));
                RUNTIME_LOG("[triage-waiter] visit=" << triageCount << " ra=0x" << std::hex << ra << " sp=0x" << sp << std::dec);
            }
            // 2026-10-06n (desbloqueo): volcado único del ÍTEM (a+0..15),
            // tabla maestra (s1=gp+0x5F8)[0..1] y 0x61C. w2 (ítem+8) vuelve
            // siempre a 65: hay que fijar el MASTER (tabla?), no el slot.
            {
                static bool s_itemDump = false;
                if (!s_itemDump) {
                    s_itemDump = true;
                    const uint32_t s1 = (gp + 0x5F8u) & 0x01FFFFFFu;
                    uint32_t iw[4] = {}, t0 = 0, t1 = 0, v61c = 0;
                    const uint32_t pa = (a & 0x01FFFFFFu);
                    if (pa + 16 <= 0x02000000u)
                        for (int i = 0; i < 4; ++i)
                            std::memcpy(&iw[i], rdram + pa + i * 4, 4);
                    if (s1 + 8 <= 0x02000000u) {
                        std::memcpy(&t0, rdram + s1, 4);
                        std::memcpy(&t1, rdram + s1 + 4, 4);
                    }
                    std::memcpy(&v61c, rdram + ((gp + 0x61Cu) & 0x01FFFFFFu), 4);
                    RUNTIME_LOG("[triage-item] a=0x" << std::hex << a << " item=" << iw[0] << " " << iw[1] << " " << iw[2] << " " << iw[3]
                                << " table=" << t0 << " " << t1 << " 61c=0x" << v61c << std::dec);
                }
            }
            std::memcpy(rdram + ((gp + 0x638u) & 0x01FFFFFFu), &b, 4);
            // 2026-10-06v (BASELINE): completar-ítem DESACTIVADO (sin efecto
            // probado). Se deja el decremento original v7 (clasificatorio).
            (void)0;
            // v7 2026-10-05 (visual-primero): v6 (poner en cero) no cambió
            // nada porque el flip republica desde tabla. Nuevo enfoque: el
            // ítem lleva contador w2 (0x40/0x41 = bloques restantes). El
            // consumidor real lo decrementaría al reproducir; sin motor SPU
            // nunca baja y el productor reintenta eternamente. Simular
            // consumo: decrementar w2 por visita hasta 0 ("stream completo
            // silencioso"). Si el productor avanza al llegar a 0,
            // breakthrough hacia menú. TRIAGE.
            {
                const uint32_t pa = (a & 0x01FFFFFFu);
                if (pa + 16 <= 0x02000000u) {
                    uint32_t w2 = 0;
                    std::memcpy(&w2, rdram + pa + 8, 4);
                    if (w2 > 0 && w2 <= 0x10000u) {
                        const uint32_t dec = w2 - 1;
                        std::memcpy(rdram + pa + 8, &dec, 4);
                        if (dec == 0 || (dec & 15u) == 0u) {
                            RUNTIME_LOG("[triage-consume] item blocks left=" << dec);
                        }
                    }
                }
            }
            const uint8_t zero = 0, one = 1;
            std::memcpy(rdram + ((gp + 0x630u) & 0x01FFFFFFu), &zero, 1);
            // v3 2026-10-04: flag640==0 hace que el poller 0x6e6a58 retorne
            // sin llamar al worker 0x6e4cc0 (1 ítem pendiente eterno). Se
            // levanta para dejarlo pasar; TRIAGE, no fix real.
            std::memcpy(rdram + ((gp + 0x640u) & 0x01FFFFFFu), &one, 1);
            ctx->r[3] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<int32_t>(b)));
            ctx->r[2] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<int32_t>(b)));
            ctx->r[16] = _mm_setzero_si128(); // $s0 = 0
            ctx->r[17] = _mm_cvtsi32_si128(gp + 0x5F8u); // $s1 = master table base
            ctx->pc = 0x6e4df4u; // Salto directo al inicio del flip nativo (saltea spin de Status)
            // v8 2026-10-05 (visual-primero): instalar el consumidor. El
            // slot cb (*(gp-0x7A30)) lleva 0 desde el arranque: el juego lo
            // instala vía 0x6e6930 solo cuando algo está listo, y el cleanup
            // 0x6e6cb8 lo vuelve a 0 tras cada uso. Como ese "algo" nunca
            // llega, se instala el poller (0x6e6a58, despachador natural del
            // spin) cada vez que el slot está en 0 (se respetan installs
            // propios != 0) y el código real (poller → worker 0x6e4cc0)
            // decide aguas abajo con los flags ya forzados. Si el worker
            // necesita algo ausente, el nuevo pc lo dirá. TRIAGE.
            {
                const uint32_t cbAddr = (gp - 0x7A30u) & 0x01FFFFFFu;
                uint32_t cur = 0;
                std::memcpy(&cur, rdram + cbAddr, 4);
                if (cur == 0u) {
                    const uint32_t poller = 0x006e6a58u;
                    std::memcpy(rdram + cbAddr, &poller, 4);
                    static bool loggedCb = false;
                    if (!loggedCb) {
                        loggedCb = true;
                        RUNTIME_LOG("[triage-cb] installed poller 0x6e6a58 as consumer");
                    }
                }
                // 2026-10-06r (desbloqueo): ¿qué consumer pone el juego?
                // Si cur != 0 siempre (0x6e6930 instaló el suyo), el poller
                // propio nunca corre y el cb del juego es quien no retorna.
                else {
                    static int s_cbSeen = 0;
                    if (++s_cbSeen <= 5) {
                        RUNTIME_LOG("[triage-cb] game consumer=0x" << std::hex << cur << std::dec << " n=" << s_cbSeen);
                    }
                }
            }
        };
        // 2026-10-06 (visual-primero, ANTI-CÍRCULOS): se DESACTIVA el
        // override del spin para ver si, con todo lo real en pie (LIBSD
        // físico, servidor 0x573, transfers híbridas, ENDX lleno), el spin
        // sale SOLO. Si el juego avanza sin forzarlo, los triages eran el
        // bloqueante (iatrogenia) y se repiensa todo. Si se queda clavado
        // en el primer 0x6e4d98, el triage era necesario y se reactiva.
        // VEREDICTO 2026-10-06 (smoke 5 min sin triage): se clava en el
        // PRIMER 0x6e4d98 (85 hits ahí vs 5 en 0x6e4cdc) con transfers
        // fluyendo (390). Iatrogenia REFUTADA: el forzado es necesario
        // (por ahora) y no envenena. Reactivado.
        runtime.replaceFunction(0x6e4d98u, triage6e4d98);

        // 2026-10-06z: registrar flips reales en 0x6e4e1c (sb $v0, 0x604($gp))
        static auto triageFlipLog = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t gp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[28]));
            const uint8_t val = static_cast<uint8_t>(_mm_cvtsi128_si32(ctx->r[2]));
            std::memcpy(rdram + ((gp + 0x604u) & 0x01FFFFFFu), &val, 1);
            static int s_flip = 0;
            if (++s_flip <= 10 || s_flip % 100 == 1) {
                RUNTIME_LOG("[triage-flow] FLIP EXECUTED! toggle=" << (unsigned)val << " n=" << s_flip);
            }
            ctx->pc = 0x6e4e20u;
        };
        runtime.replaceFunction(0x6e4e1cu, triageFlipLog);
        // 2026-10-07h (completado REAL de endfn-6279D0): el stub anterior solo
        // emulaba el primer move y parkaba en 0x6279d4 (sin cobertura
        // generada) → 4C38D8 (memcpy del completado) JAMÁS corría. Ahora
        // cuerpo exacto: a1=a0; sp-=0x10; sd ra; a0=0xa44bc0; a2=0x120
        // (delay); call 4C38D8. Continuación en 0x6279ec registrado aparte
        // (sync/ei/epílogo/jr ra). Sin truncados ni missing-functions.
        static auto triageCb9d0 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            const uint64_t sp0 = static_cast<uint64_t>(_mm_cvtsi128_si64(ctx->r[29]));
            const uint32_t sp = static_cast<uint32_t>(sp0 - 0x10u);
            ctx->r[29] = _mm_cvtsi64_si128(static_cast<int64_t>(sp));
            const uint64_t ra = static_cast<uint64_t>(_mm_cvtsi128_si64(ctx->r[31]));
            if ((sp & 0x01FFFFFFu) + 8 <= 0x02000000u) {
                std::memcpy(rdram + (sp & 0x01FFFFFFu), &ra, 8);
            }
            ctx->r[5] = ctx->r[4]; // move a1,a0
            ctx->r[4] = _mm_cvtsi64_si128(static_cast<int64_t>(0x00a44bc0)); // lui+addiu a0
            ctx->r[6] = _mm_cvtsi64_si128(static_cast<int64_t>(0x120)); // delay: a2=0x120
            ctx->r[31] = _mm_cvtsi64_si128(static_cast<int64_t>(0x6279ecu)); // ra del call
            static int s_cb = 0;
            if (++s_cb <= 5 || s_cb % 200 == 1) {
                RUNTIME_LOG("[triage-cb9d0] endfn-6279D0 call 4C38D8 n=" << s_cb);
            }
            if (!runtime->dispatchGuestBranch(rdram, ctx, 0x4C38D8u, 0x6279E4u, 0x6279ECu, PS2Runtime::GuestBranchKind::DirectCall, "JAL")) {
                // Checkpoint-due: correr natural vía scheduler (con sus yields),
                // retorno a 0x6279ec (continuación registrada abajo).
                ctx->pc = 0x4C38D8u;
                return;
            }
            ctx->pc = 0x6279ecu;
        };
        runtime.replaceFunction(0x6279d0u, triageCb9d0);
        // Continuación post-call de 6279D0: sync(nop) / ei / ld ra,(sp) /
        // sp+=0x10 / jr ra. Exacta.
        static auto triageCb9d0cont = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            ctx->cop0_status |= 0x10000; // ei
            const uint32_t sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));
            uint64_t ra = 0;
            if ((sp & 0x01FFFFFFu) + 8 <= 0x02000000u) {
                std::memcpy(&ra, rdram + (sp & 0x01FFFFFFu), 8);
            }
            ctx->r[29] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<uint64_t>(sp) + 0x10u));
            ctx->pc = static_cast<uint32_t>(ra);
        };
        runtime.replaceFunction(0x6279ecu, triageCb9d0cont);
        // 2026-10-07j (detector de retorno de 51FEA0): 0x4c0210 (`j 0x4c1900`)
        // NO tiene cobertura generada → si 51FEA0 retorna ahí, el thread
        // muere en missing-function. Este hook lo detecta (log) y emula el
        // salto a main (move a0,v0 ya hecho por el delay slot... no: el hook
        // arranca EN 0x4c0210, el delay 0x4c0214 no corrió; emular ambos).
        static auto triage4c0210 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram;
            const uint32_t v0 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[2]));
            ctx->r[4] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<int32_t>(v0))); // move a0,v0
            static int s_210 = 0;
            RUNTIME_LOG("[triage-210] 4C0210 reached (51FEA0 returned) n=" << ++s_210 << " v0=0x" << std::hex << v0 << std::dec);
            ctx->pc = 0x4C1900u;
            (void)runtime;
        };
        runtime.replaceFunction(0x4c0210u, triage4c0210);
        // 2026-10-09 (Fase 0): probes SIF init/exit del juego.
        g_origCf158 = runtime.lookupFunction(0x4cf158u);
        if (g_origCf158) runtime.replaceFunction(0x4cf158u, probeCf158wrap);
        g_origD6380 = runtime.lookupFunction(0x4d6380u);
        if (g_origD6380) runtime.replaceFunction(0x4d6380u, probeD6380wrap);
        g_origCf360 = runtime.lookupFunction(0x4cf360u);
        if (g_origCf360) runtime.replaceFunction(0x4cf360u, probeCf360wrap);
        g_origCfc60 = runtime.lookupFunction(0x4cfc60u);
        if (g_origCfc60) runtime.replaceFunction(0x4cfc60u, probeCfc60wrap);
        // 2026-10-09 (cadena del burst): ¿el recomp ejecuta la cadena
        // FUN_004d19c8 -> FUN_004d0cd0 / FUN_004d3730? (FUN_004d3730 trae el
        // handshake SIF en 0x4d3740 que el real ejecuta en `ss01`). Env SWORD_CHAIN=1.
        if (std::getenv("SWORD_CHAIN") != nullptr)
        {
            static PS2Runtime::RecompiledFunction s_o19c8 = nullptr;
            static PS2Runtime::RecompiledFunction s_ocd0 = nullptr;
            static PS2Runtime::RecompiledFunction s_o3730 = nullptr;
            static auto wrap19c8 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
                static uint64_t n = 0; ++n;
                if (n <= 4u || (n % 2000u) == 0u) std::cerr << "[chain] 4d19c8 n=" << n << std::endl;
                if (s_o19c8) s_o19c8(rdram, ctx, runtime);
            };
            static auto wrapcd0 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
                static uint64_t n = 0; ++n;
                if (n <= 4u || (n % 2000u) == 0u) std::cerr << "[chain] 4d0cd0 n=" << n << std::endl;
                if (s_ocd0) s_ocd0(rdram, ctx, runtime);
            };
            static auto wrap3730 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
                static uint64_t n = 0; ++n;
                if (n <= 4u || (n % 2000u) == 0u) std::cerr << "[chain] 4d3730 n=" << n << std::endl;
                if (s_o3730) s_o3730(rdram, ctx, runtime);
            };
            s_o19c8 = runtime.lookupFunction(0x4d19c8u);
            if (s_o19c8) runtime.replaceFunction(0x4d19c8u, wrap19c8);
            s_ocd0 = runtime.lookupFunction(0x4d0cd0u);
            if (s_ocd0) runtime.replaceFunction(0x4d0cd0u, wrapcd0);
            s_o3730 = runtime.lookupFunction(0x4d3730u);
            std::cerr << "[chain] install: o19c8=" << (s_o19c8 != nullptr)
                      << " ocd0=" << (s_ocd0 != nullptr)
                      << " o3730=" << (s_o3730 != nullptr) << std::endl;
            if (s_o3730) runtime.replaceFunction(0x4d3730u, wrap3730);

            // Helpers del loop de carga del hilo 3 (0x629644..0x6297f0): loguear su
            // retorno ($v0), que es el que controla los retries `bne v0,1`.
            // (RecompiledFunction es puntero a función ⇒ lambdas SIN captura.)
            static PS2Runtime::RecompiledFunction s_oee38 = nullptr;
            static PS2Runtime::RecompiledFunction s_ofa80 = nullptr;
            static PS2Runtime::RecompiledFunction s_of860 = nullptr;
            static auto probeEe38 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
                if (s_oee38) s_oee38(rdram, ctx, runtime);
                std::cerr << "[h] 4cee38 ret=0x" << std::hex
                          << static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[2])) << std::dec << std::endl;
            };
            static auto probeFa80 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
                if (s_ofa80) s_ofa80(rdram, ctx, runtime);
                std::cerr << "[h] 4cfa80 ret=0x" << std::hex
                          << static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[2])) << std::dec << std::endl;
            };
            static auto probeF860 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
                if (s_of860) s_of860(rdram, ctx, runtime);
                std::cerr << "[h] 4cf860 ret=0x" << std::hex
                          << static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[2])) << std::dec << std::endl;
            };
            s_oee38 = runtime.lookupFunction(0x4cee38u);
            if (s_oee38) runtime.replaceFunction(0x4cee38u, probeEe38);
            s_ofa80 = runtime.lookupFunction(0x4cfa80u);
            if (s_ofa80) runtime.replaceFunction(0x4cfa80u, probeFa80);
            s_of860 = runtime.lookupFunction(0x4cf860u);
            if (s_of860) runtime.replaceFunction(0x4cf860u, probeF860);
        }
        // 2026-10-07s: activar wrap del VBlank para perfil de threads.
        g_orig6e45d8 = runtime.lookupFunction(0x6e45d8u);
        runtime.replaceFunction(0x6e45d8u, triage6e45d8wrap);
        // 2026-10-06z (C0-wait, REVERSIBLE): el worker se estaciona dentro
        // de 6E4848 sin llegar al spin; únicos loops sin syscall: bc0f en
        // 0x6e4d24 y 0x6e4ac4 (flag C0 que nada pone). Como nuestro SPR-DMA
        // es síncrono (ya completo al escribir CHCR), forzar fallthrough
        // es equivalencia exacta, no trampa. Si el worker avanza (spin!) →
        // era ese wait.
        // (un hook por sitio, sin captura)
        static auto triageBc0fA = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            static int s_bc = 0;
            if (++s_bc <= 4) {
                RUNTIME_LOG("[triage-bc0f] 0x6e4d24 fallthrough n=" << s_bc);
            }
            ctx->pc = 0x6e4d2cu;
        };
        runtime.replaceFunction(0x6e4d24u, triageBc0fA);
        static auto triageBc0fB = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            static int s_bc = 0;
            if (++s_bc <= 4) {
                RUNTIME_LOG("[triage-bc0f] 0x6e4ac4 fallthrough n=" << s_bc);
            }
            ctx->pc = 0x6e4ad0u;
        };
        runtime.replaceFunction(0x6e4ac4u, triageBc0fB);
        // 2026-10-06z3 (arrivals?, emulación EXACTA): 0x6b16b0 llama a
        // 0x627F50 (44++) si a1>0; nadie lo llama directo (¿handler de
        // interrupción?). Loguear entradas + args. blez exacto.
        static auto triage6b16b0 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            const int32_t a1 = static_cast<int32_t>(_mm_cvtsi128_si32(ctx->r[5]));
            const uint32_t t0 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[12]));
            static int s_b16 = 0;
            if (++s_b16 <= 5 || s_b16 % 500 == 1) {
                RUNTIME_LOG("[triage-6b] 6B16B0 entry a1=" << a1 << " n=" << s_b16);
            }
            // delay slot: sll a3,a1,16
            ctx->r[7] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<int32_t>(
                static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5])) << 16)));
            (void)t0;
            ctx->pc = (a1 <= 0) ? 0x6b16d0u : 0x6b16b8u;
        };
        runtime.replaceFunction(0x6b16b0u, triage6b16b0);
        // Y la entrada 0x627f50 (44++): lw exacto + log args.
        static auto triage627f50 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t gp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[28]));
            uint32_t w = 0;
            std::memcpy(&w, rdram + ((gp + 0x44u) & 0x01FFFFFFu), 4);
            ctx->r[2] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<int32_t>(w))); // lw exacto
            static int s_f50 = 0;
            if (++s_f50 <= 5) {
                RUNTIME_LOG("[triage-6b] 627F50 entry 44=" << w << " n=" << s_f50);
            }
            ctx->pc = 0x627f54u;
        };
        // 2026-10-12: DESACTIVADO. Este stub de triage abandonaba FUN_00627f50 a la mitad
        // (pc=0x627F54, un PC intermedio) -> el scheduler no podia reanudar -> makeDormant
        // -> EE clavado y VRAM vacia. Y ademas pisaba al fix627f50 (se instalaba despues).
        // SWORD_TRIAGE_627F50=1 para reactivarlo.
        if (std::getenv("SWORD_TRIAGE_627F50") != nullptr) {
            runtime.replaceFunction(0x627f50u, triage627f50);
        }
        // 2026-10-07g (muestreo a/b, wrap del original sin emulación): hook en
        // la entrada REAL de 6e4848 (jal target → sí dispara). Guarda el
        // original ANTES de reemplazar (global: la lambda no captura).
        // Loguea gates gp+5ec/610/614/638/63c cada 300 visitas + en cada FLIP.
        g_orig4848 = runtime.lookupFunction(0x6e4848u);
        // 2026-10-08 TRIAGE: hookear el init del motor de DMA si está registrado.
        {
            g_orig6e68e0 = runtime.lookupFunction(0x6e68e0u);
            g_orig6e6934 = runtime.lookupFunction(0x6e6934u);
            RUNTIME_LOG("[motorinit] lookup 0x6e68e0=" << (g_orig6e68e0 ? "OK" : "NULL")
                        << " 0x6e6934=" << (g_orig6e6934 ? "OK" : "NULL"));
            if (g_orig6e68e0 != nullptr) {
                runtime.replaceFunction(0x6e68e0u, triage6e68e0hook);
            }
            if (g_orig6e6934 != nullptr) {
                runtime.replaceFunction(0x6e6934u, triage6e6934hook);
            }
            g_orig6e6640 = runtime.lookupFunction(0x6e6640u);
            RUNTIME_LOG("[motorinit] lookup 0x6e6640=" << (g_orig6e6640 ? "OK" : "NULL"));
            if (g_orig6e6640 != nullptr) {
                runtime.replaceFunction(0x6e6640u, triage6e6640hook);
            }
            g_orig4d1a60 = runtime.lookupFunction(0x4d1a60u);
            RUNTIME_LOG("[motorinit] lookup 0x4d1a60=" << (g_orig4d1a60 ? "OK" : "NULL"));
            if (g_orig4d1a60 != nullptr) {
                runtime.replaceFunction(0x4d1a60u, triage4d1a60hook);
            }
            g_orig6efd28 = runtime.lookupFunction(0x6efd28u);
            RUNTIME_LOG("[motorinit] lookup 0x6efd28=" << (g_orig6efd28 ? "OK" : "NULL"));
            if (g_orig6efd28 != nullptr) {
                runtime.replaceFunction(0x6efd28u, triage6efd28hook);
            }
        }
        runtime.replaceFunction(0x6e4848u, triage4848hook);
        static auto triage4c1970 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t s4 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[20]));
            uint32_t a0 = 0, v0 = 0;
            if (s4 + 0x1BF0u + 4 <= 0x02000000u) {
                std::memcpy(&a0, rdram + ((s4 + 0x1BF0u) & 0x01FFFFFFu), 4);
                if (a0 + 0x3C + 4 <= 0x02000000u) {
                    std::memcpy(&v0, rdram + ((a0 + 0x3C) & 0x01FFFFFFu), 4);
                }
            }
            static int s_1970 = 0;
            if (++s_1970 <= 8) {
                RUNTIME_LOG("[triage-1970] 4C1970 entry n=" << s_1970 << " s4=0x" << std::hex << s4 << " a0=0x" << a0 << " fn=0x" << v0);
            }
            // Emulación exacta: las 2 lw (el jr indirecto queda para el código real).
            ctx->r[4] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<int32_t>(a0)));
            ctx->r[2] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<int32_t>(v0)));
            ctx->pc = 0x4c1978u;
        };
        runtime.replaceFunction(0x4c1970u, triage4c1970);
        // 2026-10-07b: 4c1970 es INTERIOR de FUN_004c1900 (callback-dispatch
        // loop: recorre lista en *(s4+1BF0)+148 con jalr, luego dispatch en
        // 1970). Hookear la ENTRADA 0x4c1900: emulación exacta del prólogo +
        // log de s2 (¿lista vacía→1970 directo? ¿loop de callbacks?).
        static auto triage4c1900 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            auto st64 = [&](uint32_t addr, uint64_t v) {
                addr &= 0x01FFFFFFu;
                if (addr + 8 <= 0x02000000u) std::memcpy(rdram + addr, &v, 8);
            };
            auto ld32 = [&](uint32_t addr) -> uint32_t {
                uint32_t v = 0;
                addr &= 0x01FFFFFFu;
                if (addr + 4 <= 0x02000000u) std::memcpy(&v, rdram + addr, 4);
                return v;
            };
            const uint64_t sp0 = static_cast<uint64_t>(_mm_cvtsi128_si64(ctx->r[29]));
            const uint32_t sp = static_cast<uint32_t>(sp0 - 0x30u);
            ctx->r[29] = _mm_cvtsi64_si128(static_cast<int64_t>(sp));
            st64(sp + 0x20u, static_cast<uint64_t>(_mm_cvtsi128_si64(ctx->r[20]))); // s4
            st64(sp + 0x18u, static_cast<uint64_t>(_mm_cvtsi128_si64(ctx->r[19]))); // s3
            st64(sp + 0x00u, static_cast<uint64_t>(_mm_cvtsi128_si64(ctx->r[16]))); // s0
            st64(sp + 0x08u, static_cast<uint64_t>(_mm_cvtsi128_si64(ctx->r[17]))); // s1
            st64(sp + 0x10u, static_cast<uint64_t>(_mm_cvtsi128_si64(ctx->r[18]))); // s2
            st64(sp + 0x28u, static_cast<uint64_t>(_mm_cvtsi128_si64(ctx->r[31]))); // ra
            ctx->r[20] = _mm_cvtsi64_si128(static_cast<int64_t>(0x00870000)); // lui s4,0x87
            const uint32_t v0 = ld32(0x00870000u + 0x1BF0u);
            const uint32_t s2 = ld32(v0 + 0x148u);
            ctx->r[2] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<int32_t>(v0)));
            ctx->r[18] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<int32_t>(s2)));
            ctx->r[19] = ctx->r[4]; // delay slot: daddu s3,a0
            static int s_1900 = 0;
            if (++s_1900 <= 6) {
                RUNTIME_LOG("[triage-1900] 4C1900 entry n=" << s_1900 << " a0=0x" << std::hex << static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) << " s2(list)=0x" << s2 << std::dec);
            }
            // 2026-10-07d: no delegar el dispatch al interior (los hooks no
            // disparan ahí): emular TAMBIÉN 1970/1974 (2 lw) y saltar DIRECTO
            // a la worker-fn con dispatchGuestBranch. Loguea a0/fn.
            {
                const uint32_t a0 = ld32(0x00870000u + 0x1BF0u);
                const uint32_t fn = ld32(a0 + 0x3Cu);
                ctx->r[4] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<int32_t>(a0)));
                ctx->r[2] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<int32_t>(fn)));
                ctx->pc = 0x4c1978u;
                RUNTIME_LOG("[triage-1900] dispatch worker a0=0x" << std::hex << a0 << " fn=0x" << fn << std::dec);
                if (fn != 0u) {
                    (void)runtime->dispatchGuestBranch(rdram, ctx, fn, 0x4c1978u, 0x4c197cu, PS2Runtime::GuestBranchKind::IndirectJump, "JR-V0");
                    return;
                }
                RUNTIME_LOG("[triage-1900] worker fn NULL, parking at 0x4c1978");
            }
            ctx->pc = (s2 == 0) ? 0x4c1970u : 0x4c1930u;
        };
        runtime.replaceFunction(0x4c1900u, triage4c1900);
        static auto triageProd = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t v0 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[2]));
            const uint32_t gp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[28]));
            // TRIAGE 2026-10-08: estado de la cola del motor ANTES de encolar.
            auto rd32a = [&](uint32_t addr) -> uint32_t {
                uint32_t v = 0;
                const uint32_t p = addr & 0x01FFFFFFu;
                if (p + 4u <= 0x02000000u) std::memcpy(&v, rdram + p, 4);
                return v;
            };
            auto rd8a = [&](uint32_t addr) -> uint32_t {
                const uint32_t p = addr & 0x01FFFFFFu;
                return (p < 0x02000000u) ? rdram[p] : 0u;
            };
            const uint32_t headPrev = rd32a(gp + 0x638u);
            const uint32_t tail = rd32a(gp + 0x63Cu);
            const uint32_t cb = rd32a(gp - 0x7A30u);
            std::memcpy(rdram + ((gp + 0x638u) & 0x01FFFFFFu), &v0, 4);
            static int s_prod = 0;
            ++s_prod;
            if (s_prod <= 40 || (s_prod % 200) == 0) {
                RUNTIME_LOG("[prod] n=" << s_prod
                            << " headPrev=0x" << std::hex << headPrev
                            << " newHead=0x" << v0
                            << " tail=0x" << tail << std::dec
                            << " f630=" << rd8a(gp + 0x630u)
                            << " f5cf=" << rd8a(gp + 0x5CFu)
                            << " cb=0x" << std::hex << cb << std::dec);
            }
            ctx->pc = 0x6e434cu;
        };
        runtime.replaceFunction(0x6e4348u, triageProd);

        // 2026-10-06l (desbloqueo, temporal): BYPASS de 6E4ED8 (submitter
        // SPU del loop: item, (n<<12)|0x42). Sus callers IGNORAN v0 (usan el
        // contador s0), así que retornar 0 inmediato es seguro de probar:
        // si el worker avanza → el waiter SPU vive DENTRO de 6E4ED8 (y se
        // puede dejar bypasseado en silencio para v0.1.0). Si igual →
        // revertir (el waiter está en otro lado).
        static auto triage6e4ed8 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            const uint32_t a0 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            const uint32_t a1 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
            static int s_ed8 = 0;
            if (++s_ed8 <= 3) {
                RUNTIME_LOG("[triage-ed8] 6E4ED8 bypass a0=0x" << std::hex << a0 << " a1=0x" << a1 << std::dec << " n=" << s_ed8);
            }
            ctx->r[2] = _mm_setzero_si128(); // v0 = 0
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x6e4ed8u, triage6e4ed8);

        // Save presentation frame screenshot helper
        static auto savePresentationBmp = [](PS2Runtime *rt, const char *filename) {
            if (!rt) return;
            rt->gs().latchHostPresentationFrame();
            std::vector<uint8_t> rgba;
            uint32_t width = 0, height = 0;
            if (!rt->gs().copyLatchedHostPresentationFrame(rgba, width, height) || rgba.empty() || width == 0 || height == 0) {
                std::cout << "[screenshot] copyLatchedHostPresentationFrame returned false or empty" << std::endl;
                return;
            }
            std::ofstream out(filename, std::ios::binary);
            if (!out.is_open()) {
                std::cout << "[screenshot] Failed to open file for write: " << filename << std::endl;
                return;
            }

            const uint32_t rowSize = width * 4;
            const uint32_t imageSize = rowSize * height;
            const uint32_t fileSize = 54 + imageSize;

            uint8_t header[54] = {
                'B', 'M',
                static_cast<uint8_t>(fileSize), static_cast<uint8_t>(fileSize >> 8), static_cast<uint8_t>(fileSize >> 16), static_cast<uint8_t>(fileSize >> 24),
                0, 0, 0, 0,
                54, 0, 0, 0,
                40, 0, 0, 0,
                static_cast<uint8_t>(width), static_cast<uint8_t>(width >> 8), static_cast<uint8_t>(width >> 16), static_cast<uint8_t>(width >> 24),
                static_cast<uint8_t>(height), static_cast<uint8_t>(height >> 8), static_cast<uint8_t>(height >> 16), static_cast<uint8_t>(height >> 24),
                1, 0,
                32, 0,
                0, 0, 0, 0,
                static_cast<uint8_t>(imageSize), static_cast<uint8_t>(imageSize >> 8), static_cast<uint8_t>(imageSize >> 16), static_cast<uint8_t>(imageSize >> 24),
                0x13, 0x0B, 0, 0,
                0x13, 0x0B, 0, 0,
                0, 0, 0, 0,
                0, 0, 0, 0
            };
            out.write(reinterpret_cast<const char*>(header), 54);

            uint32_t nonZero = 0;
            std::vector<uint8_t> row(rowSize);
            for (int y = static_cast<int>(height) - 1; y >= 0; --y) {
                const uint8_t *srcRow = rgba.data() + y * rowSize;
                for (uint32_t x = 0; x < width; ++x) {
                    row[x * 4 + 0] = srcRow[x * 4 + 2]; // B
                    row[x * 4 + 1] = srcRow[x * 4 + 1]; // G
                    row[x * 4 + 2] = srcRow[x * 4 + 0]; // R
                    row[x * 4 + 3] = srcRow[x * 4 + 3]; // A
                    if (srcRow[x * 4 + 0] != 0 || srcRow[x * 4 + 1] != 0 || srcRow[x * 4 + 2] != 0) {
                        nonZero++;
                    }
                }
                out.write(reinterpret_cast<const char*>(row.data()), rowSize);
            }
            std::cout << "[screenshot] Saved presentation frame " << width << "x" << height 
                      << " (nonZero=" << nonZero << "/" << (width * height) << ") to " << filename << std::endl;
        };

        // 0x004d3730: en el SDK de esta build es un handshake DIntr/SIF
        // (`DIntr; *0x1000F000=4; sync; poll bit2; DIntr; ...; EIntr`) — el real
        // lo ejecuta en `ss01` (pc=0x4d3764). El stub `gsSyncV0Stub` (return 0) lo
        // saltea. Con SWORD_FIX_4D3740=1 NO se instala el stub: corre el cuerpo
        // real generado + la continuación 0x4d3740 + `entry_004d3770`.
        static const bool s_real4d3730 = []() {
            const char *e = std::getenv("SWORD_FIX_4D3740");
            return e != nullptr && *e != '0';
        }();
        static auto gsSyncV0Stub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->r[2] = _mm_setzero_si128();
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        if (!s_real4d3730) {
            runtime.replaceFunction(0x004d3730u, gsSyncV0Stub);
        }

        // 0x004d37c0: sceGsSyncV(1) / VSync wait with field/tick return
        static auto gsSyncV1Stub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram;
            static uint64_t s_syncTick = 0;
            s_syncTick++;
            if (s_syncTick == 1 || s_syncTick == 10 || s_syncTick == 30 || s_syncTick == 60 || s_syncTick == 120 ||
                s_syncTick == 300 || (s_syncTick % 600) == 0) {
                const std::string filename = "/home/lubonch/repos/Sword-Of-Etheria-PC-Recomp/work/build-game/frame_" + std::to_string(s_syncTick) + ".bmp";
                savePresentationBmp(runtime, filename.c_str());
            }
            const uint64_t fieldBit = (s_syncTick & 1ull) << 13; // bit 13 alternates 0 / 1 (even / odd field)
            const uint64_t retVal = fieldBit;
            ctx->r[2] = _mm_cvtsi64_si128(static_cast<int64_t>(retVal));
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d37c0u, gsSyncV1Stub);

        // 0x004d37e0: FUN_004d37c0 continuation stub
        static auto returnStub4d37e0 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            const uint32_t sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));
            if ((sp & 0x01FFFFFFu) + 0x20 <= 0x02000000u) {
                uint64_t raVal = 0, v0Val = 0;
                std::memcpy(&raVal, rdram + ((sp + 0x10) & 0x01FFFFFFu), 8);
                std::memcpy(&v0Val, rdram + ((sp + 0x08) & 0x01FFFFFFu), 8);
                ctx->r[31] = _mm_cvtsi64_si128(raVal);
                ctx->r[2] = _mm_cvtsi64_si128(v0Val);
            }
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x20);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d37e0u, returnStub4d37e0);

        // 0x4d3730 / 0x4d3740 / 0x4d0cd0 / 0x4d3720 / 0x4d3700 / 0x4d3710:
        // el cluster truncado `0x4d3700-0x4d3770` (y `0x4d0cd0`) se corrigió en el
        // CSV de Ghidra y se REGENERÓ en `work/generated` (ver decisions.md 15/16),
        // así que ya no hacen falta overrides manuales: el cuerpo real de
        // `FUN_004d3730` (handshake DIntr/`0x1000F000`/poll + epílogo) está generado.
        // El stub `gsSyncV0Stub` de 0x4d3730 (más abajo) lo conserva salvo que
        // `SWORD_FIX_4D3740=1`, con lo que corre el cuerpo real + el modelo de
        // `0x1000F000` en `ps2_memory.cpp`.

        // 0x4d0cd0 / 0x4d3720: getters truncados por Ghidra (CSV End cortado antes
        // del `jr $ra`). Corregidos en el CSV y regenerados (`FUN_004d0cd0` ahora
        // devuelve 0x00873C90; `FUN_004d3720` escribe `0x873D60`), así que ya no se
        // necesitan overrides. Ver `decisions.md` 15/16.

        // 0x004d3b98: FlushCache(int direction) -> no-op on PC, return 1 to $ra
        static auto flushCacheStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->r[2] = _mm_cvtsi32_si128(1);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d3b98u, flushCacheStub);

        // 0x004d3af0 / 0x004d3b04: EE cache/TLB invalidation helper -> no-op on PC, return to $ra
        static auto cacheFlushHelperStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d3af0u, cacheFlushHelperStub);
        runtime.replaceFunction(0x004d3b04u, cacheFlushHelperStub);

        // 0x004d3c0c: FlushCache epilogue return stub -> jr $ra; addiu $sp, $sp, 0x40
        static auto returnStub4d3c0c = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x40);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d3c0cu, returnStub4d3c0c);

        // 0x004d0ce0 / 0x004d0d28: sceGsResetPath / VIF1 reset & init packet
        static auto gsResetPathStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            RUNTIME_LOG("[game_overrides] sceGsResetPath (0x4d0ce0/0x4d0d28) executed.");
            if (runtime) {
                runtime->Store32(rdram, ctx, 0x10003C10u, 1);
                runtime->Store32(rdram, ctx, 0x10003C20u, 2);
                ctx->vu0_fbrst = (ctx->vu0_fbrst | 0x200) & 0x00000C0Cu;
            }
            const uint32_t a1 = 0x00873ca0u;
            const uint32_t a2 = 0x10005000u;
            if ((a1 & 0x01FFFFFFu) + 32 <= 0x02000000u) {
                __m128i q0 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(rdram + (a1 & 0x01FFFFFFu)));
                __m128i q1 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(rdram + ((a1 + 16) & 0x01FFFFFFu)));
                ctx->r[4] = q0; // $a0
                ctx->r[2] = q1; // $v0
                if (runtime) {
                    runtime->Store128(rdram, ctx, a2, q0);
                    runtime->Store128(rdram, ctx, a2, q1);
                    runtime->Store32(rdram, ctx, 0x10003000u, 1);
                }
            }
            ctx->r[5] = _mm_cvtsi32_si128(a1);
            ctx->r[6] = _mm_cvtsi32_si128(a2);
            ctx->r[7] = _mm_cvtsi32_si128(1);
            ctx->r[3] = _mm_cvtsi32_si128(0x10003000u);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31])); // jr $ra
        };
        runtime.replaceFunction(0x004d0ce0u, gsResetPathStub);
        runtime.replaceFunction(0x004d0d28u, gsResetPathStub);

        // 0x004d0478: sceDmaReset(mode in $a0) -> returns 0 in $v0
        static auto dmaResetStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            RUNTIME_LOG("[game_overrides] sceDmaReset (0x4d0478) mode=" << _mm_cvtsi128_si32(ctx->r[4]));
            ps2_stubs::sceDmaReset(rdram, ctx, runtime);
            ctx->r[2] = _mm_setzero_si128();
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d0478u, dmaResetStub);
        runtime.replaceFunction(0x004d04b4u, dmaResetStub);

        // 0x004d0558: sceDmaGetEnv(env in $a0)
        static auto dmaGetEnvStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            RUNTIME_LOG("[game_overrides] sceDmaGetEnv (0x4d0558)");
            ps2_stubs::sceDmaGetEnv(rdram, ctx, runtime);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d0558u, dmaGetEnvStub);

        // 0x004d05ec: sceDmaPutEnv(env in $a0)
        static auto dmaPutEnvStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            RUNTIME_LOG("[game_overrides] sceDmaPutEnv (0x4d05ec)");
            ps2_stubs::sceDmaPutEnv(rdram, ctx, runtime);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d05ecu, dmaPutEnvStub);

        // 0x004d02f8: sceDmaSync(chan in $a0, mode in $a1, timeout in $a2) -> returns 0 in $v0
        static auto dmaSyncStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->r[2] = _mm_setzero_si128();
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d02f8u, dmaSyncStub);

        // 0x004d0360: Epilogue of sceDmaSync fallback -> jr $ra; addiu $sp, $sp, 0x40
        static auto returnStub4d0360 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x40);
            ctx->r[2] = _mm_setzero_si128();
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d0360u, returnStub4d0360);

        // 0x004d03f0: sceDmaPhysAddr / DmaAddr(addr in $a0) -> returns physical/dma addr in $v0
        static auto dmaPhysAddrStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            const uint32_t a0 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            uint32_t v0 = a0;
            if ((a0 >> 28) == 7) {
                v0 = (a0 & 0x0fffffffu) | 0x80000000u;
                ctx->r[4] = _mm_cvtsi32_si128(v0);
            }
            ctx->r[2] = _mm_cvtsi32_si128(v0);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d03f0u, dmaPhysAddrStub);

        // 0x004d0450: sceDmaGetChan(chan_id in $a0) -> returns channel MMIO base in $v0
        static auto dmaGetChanStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            const uint32_t chanId = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            static const uint32_t s_dmaChannels[10] = {
                0x10008000u, // VIF0
                0x10009000u, // VIF1
                0x1000A000u, // GIF
                0x1000B000u, // fromIPU
                0x1000B400u, // toIPU
                0x1000C000u, // SIF0
                0x1000C400u, // SIF1
                0x1000C800u, // SIF2
                0x1000D000u, // fromSPR
                0x1000D400u  // toSPR
            };
            uint32_t ret = 0;
            if (chanId < 10) {
                ret = s_dmaChannels[chanId];
            }
            ctx->r[2] = _mm_cvtsi32_si128(ret);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d0450u, dmaGetChanStub);

        // 0x004d0468: sceDmaGetChan epilogue fallback -> jr $ra; lw $v0, 0($v1)
        static auto fallback4d0468 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t v1 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[3]));
            uint32_t v0 = 0;
            if ((v1 & 0x01FFFFFFu) + 4 <= 0x02000000u) {
                std::memcpy(&v0, rdram + (v1 & 0x01FFFFFFu), 4);
            }
            ctx->r[2] = _mm_cvtsi32_si128(v0);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d0468u, fallback4d0468);

        // 0x004d07a8: Continuation/epilogue of FUN_004d0780 (sceDmaSend chain kick & return)
        static auto returnStub4d07a8 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            const uint32_t sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));
            const uint32_t s0 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[16]));
            const uint32_t s1 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[17]));

            if (runtime && s0 != 0) {
                static uint32_t s_logCount = 0;
                if (s_logCount++ < 30) {
                    RUNTIME_LOG("[game_overrides] sceDmaSend chain kick: chan=0x" << std::hex << s0 << " tag=0x" << s1 << std::dec);
                }
                runtime->Store32(rdram, ctx, s0 + 0x30u, s1);
                runtime->Store32(rdram, ctx, s0 + 0x20u, 0);
                uint32_t chcr = runtime->Load32(rdram, ctx, s0);
                chcr = (chcr & 0xFFFFFFF3u) | 0x0105u;
                runtime->Store32(rdram, ctx, s0, chcr);
                runtime->memory().processPendingTransfers();
            }

            if ((sp & 0x01FFFFFFu) + 0x30 <= 0x02000000u) {
                uint64_t s0Val = 0, s1Val = 0, raVal = 0;
                std::memcpy(&s0Val, rdram + ((sp + 0x00) & 0x01FFFFFFu), 8);
                std::memcpy(&s1Val, rdram + ((sp + 0x10) & 0x01FFFFFFu), 8);
                std::memcpy(&raVal, rdram + ((sp + 0x20) & 0x01FFFFFFu), 8);
                ctx->r[16] = _mm_cvtsi64_si128(s0Val);
                ctx->r[17] = _mm_cvtsi64_si128(s1Val);
                ctx->r[31] = _mm_cvtsi64_si128(raVal);
            }
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x30);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d07a8u, returnStub4d07a8);

        // 0x004d0818: Continuation/epilogue of FUN_004d07e8 (sceDmaSend kick & return)
        static auto returnStub4d0818 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            const uint32_t sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));
            const uint32_t s0 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[16]));
            const uint32_t s1 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[17]));
            const uint32_t s2 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[18]));

            if (runtime && s0 != 0) {
                runtime->Store32(rdram, ctx, s0 + 0x10u, s1);
                runtime->Store32(rdram, ctx, s0 + 0x20u, s2);
                uint32_t chcr = runtime->Load32(rdram, ctx, s0);
                chcr = (chcr & 0xFFFFFFF3u) | 0x0101u;
                runtime->Store32(rdram, ctx, s0, chcr);
                runtime->memory().processPendingTransfers();
            }

            if ((sp & 0x01FFFFFFu) + 0x40 <= 0x02000000u) {
                uint64_t s0Val = 0, s1Val = 0, s2Val = 0, raVal = 0;
                std::memcpy(&s0Val, rdram + ((sp + 0x00) & 0x01FFFFFFu), 8);
                std::memcpy(&s1Val, rdram + ((sp + 0x10) & 0x01FFFFFFu), 8);
                std::memcpy(&s2Val, rdram + ((sp + 0x20) & 0x01FFFFFFu), 8);
                std::memcpy(&raVal, rdram + ((sp + 0x30) & 0x01FFFFFFu), 8);
                ctx->r[16] = _mm_cvtsi64_si128(s0Val);
                ctx->r[17] = _mm_cvtsi64_si128(s1Val);
                ctx->r[18] = _mm_cvtsi64_si128(s2Val);
                ctx->r[31] = _mm_cvtsi64_si128(raVal);
            }
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x40);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d0818u, returnStub4d0818);

        // 0x004d0890: Continuation/epilogue of FUN_004d0860 (sceDmaSend interleaved kick & return)
        static auto returnStub4d0890 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            const uint32_t sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));
            const uint32_t s0 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[16]));
            const uint32_t s1 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[17]));
            const uint32_t s2 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[18]));

            if (runtime && s0 != 0) {
                runtime->Store32(rdram, ctx, s0 + 0x10u, s1);
                runtime->Store32(rdram, ctx, s0 + 0x20u, s2);
                uint32_t chcr = runtime->Load32(rdram, ctx, s0);
                chcr = (chcr & 0xFFFFFFF3u) | 0x0109u;
                runtime->Store32(rdram, ctx, s0, chcr);
                runtime->memory().processPendingTransfers();
            }

            if ((sp & 0x01FFFFFFu) + 0x40 <= 0x02000000u) {
                uint64_t s0Val = 0, s1Val = 0, s2Val = 0, raVal = 0;
                std::memcpy(&s0Val, rdram + ((sp + 0x00) & 0x01FFFFFFu), 8);
                std::memcpy(&s1Val, rdram + ((sp + 0x10) & 0x01FFFFFFu), 8);
                std::memcpy(&s2Val, rdram + ((sp + 0x20) & 0x01FFFFFFu), 8);
                std::memcpy(&raVal, rdram + ((sp + 0x30) & 0x01FFFFFFu), 8);
                ctx->r[16] = _mm_cvtsi64_si128(s0Val);
                ctx->r[17] = _mm_cvtsi64_si128(s1Val);
                ctx->r[18] = _mm_cvtsi64_si128(s2Val);
                ctx->r[31] = _mm_cvtsi64_si128(raVal);
            }
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x40);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d0890u, returnStub4d0890);

        // 0x004d08ec: Continuation/epilogue of FUN_004d08d8 (sceDmaSendI kick & return)
        static auto returnStub4d08ec = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            const uint32_t sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));
            const uint32_t s0 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[16]));

            if (runtime && s0 != 0) {
                runtime->Store32(rdram, ctx, s0 + 0x20u, 0);
                uint32_t chcr = runtime->Load32(rdram, ctx, s0);
                chcr = (chcr & 0xFFFFFFF2u) | 0x0104u;
                runtime->Store32(rdram, ctx, s0, chcr);
                runtime->memory().processPendingTransfers();
            }

            if ((sp & 0x01FFFFFFu) + 0x20 <= 0x02000000u) {
                uint64_t s0Val = 0, raVal = 0;
                std::memcpy(&s0Val, rdram + ((sp + 0x00) & 0x01FFFFFFu), 8);
                std::memcpy(&raVal, rdram + ((sp + 0x10) & 0x01FFFFFFu), 8);
                ctx->r[16] = _mm_cvtsi64_si128(s0Val);
                ctx->r[31] = _mm_cvtsi64_si128(raVal);
            }
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x20);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d08ecu, returnStub4d08ec);

        // 0x004d0920 / 0x004d0950: sceDmaSendN(chan, src, qwc) -> kicks normal DMA
        static auto dmaSendNOverride = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            const uint32_t chan = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            const uint32_t src = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
            const uint32_t qwc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[6]));
            uint32_t phys = src;
            if ((src >> 28) == 7) {
                phys = (src & 0x0fffffffu) | 0x80000000u;
            }
            if (runtime && chan != 0) {
                static uint32_t s_logN = 0;
                if (s_logN++ < 30) {
                    RUNTIME_LOG("[game_overrides] sceDmaSendN kick: chan=0x" << std::hex << chan
                                << " src=0x" << src << " phys=0x" << phys << " qwc=" << std::dec << qwc);
                }
                runtime->Store32(rdram, ctx, chan + 0x10u, phys);
                runtime->Store32(rdram, ctx, chan + 0x20u, qwc);
                uint32_t chcr = runtime->Load32(rdram, ctx, chan);
                chcr = (chcr & 0xFFFFFFF3u) | 0x0101u;
                runtime->Store32(rdram, ctx, chan, chcr);
                runtime->memory().processPendingTransfers();
            }
            ctx->r[2] = _mm_setzero_si128();
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d0920u, dmaSendNOverride);
        runtime.replaceFunction(0x004d0950u, returnStub4d0818);

        // 0x004d09a0 / 0x004d09d0: sceDmaSendI(chan, src, qwc) -> kicks interleaved DMA
        static auto dmaSendIOverride = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            const uint32_t chan = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            const uint32_t src = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
            const uint32_t qwc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[6]));
            uint32_t phys = src;
            if ((src >> 28) == 7) {
                phys = (src & 0x0fffffffu) | 0x80000000u;
            }
            if (runtime && chan != 0) {
                static uint32_t s_logI = 0;
                if (s_logI++ < 30) {
                    RUNTIME_LOG("[game_overrides] sceDmaSendI kick: chan=0x" << std::hex << chan
                                << " src=0x" << src << " phys=0x" << phys << " qwc=" << std::dec << qwc);
                }
                runtime->Store32(rdram, ctx, chan + 0x10u, phys);
                runtime->Store32(rdram, ctx, chan + 0x20u, qwc);
                uint32_t chcr = runtime->Load32(rdram, ctx, chan);
                chcr = (chcr & 0xFFFFFFF3u) | 0x0109u;
                runtime->Store32(rdram, ctx, chan, chcr);
                runtime->memory().processPendingTransfers();
            }
            ctx->r[2] = _mm_setzero_si128();
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d09a0u, dmaSendIOverride);
        runtime.replaceFunction(0x004d09d0u, returnStub4d0890);

        // 0x004d0a50 / 0x004d0a84: Epilogue of sceDmaRecv / sceDmaRecvN -> jr $ra; addiu $sp, $sp, 0x10
        static auto returnStub4d0a50 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x10);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d0a50u, returnStub4d0a50);
        runtime.replaceFunction(0x004d0a84u, returnStub4d0a50);

        // 0x004d0a90 / 0x004d0aa8: sceDmaWatch / DMAC interrupt mask
        static auto dmaWatchStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t a0 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            uint32_t oldVal = 0;
            if ((a0 & 0x01FFFFFFu) + 4 <= 0x02000000u) {
                std::memcpy(&oldVal, rdram + (a0 & 0x01FFFFFFu), 4);
            }
            ctx->r[2] = _mm_cvtsi32_si128(oldVal);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d0a90u, dmaWatchStub);
        runtime.replaceFunction(0x004d0aa8u, dmaWatchStub);

        // 0x004d0cc8: Epilogue of sceGsSetDefDrawEnv -> jr $ra; addiu $sp, $sp, 0x50
        static auto returnStub4d0cc8 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x50);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d0cc8u, returnStub4d0cc8);

        // 0x004d0df8: sceGsSetDefDispEnv(disp, psm, w, h, dx, dy) -> returns disp in $v0
        static auto setDefDispEnvStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            const uint32_t envAddr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            RUNTIME_LOG("[game_overrides] sceGsSetDefDispEnv (0x4d0df8) env=0x" << std::hex << envAddr);
            ps2_stubs::sceGsSetDefDispEnv(rdram, ctx, runtime);
            if (envAddr != 0 && (envAddr + 40) <= 0x02000000u) {
                const uint64_t pmode = 0x66u;
                const uint64_t smode2 = 2u;
                const uint64_t bgcolor = 0u;
                std::memcpy(rdram + (envAddr & 0x01FFFFFFu), &pmode, 8);
                std::memcpy(rdram + ((envAddr + 8) & 0x01FFFFFFu), &smode2, 8);
                std::memcpy(rdram + ((envAddr + 32) & 0x01FFFFFFu), &bgcolor, 8);
            }
            ctx->r[2] = _mm_cvtsi32_si128(envAddr);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d0df8u, setDefDispEnvStub);

        // 0x004d0e5c: Fallback continuation if 0x4d0df8 was partially executed
        static auto fallback4d0e5c = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            const uint32_t sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));
            const uint32_t envAddr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[19]));
            RUNTIME_LOG("[game_overrides] sceGsSetDefDispEnv continuation (0x4d0e5c) env=0x" << std::hex << envAddr);
            ps2_stubs::sceGsSetDefDispEnv(rdram, ctx, runtime);
            if (envAddr != 0 && (envAddr + 40) <= 0x02000000u) {
                const uint64_t pmode = 0x66u;
                const uint64_t smode2 = 2u;
                const uint64_t bgcolor = 0u;
                std::memcpy(rdram + (envAddr & 0x01FFFFFFu), &pmode, 8);
                std::memcpy(rdram + ((envAddr + 8) & 0x01FFFFFFu), &smode2, 8);
                std::memcpy(rdram + ((envAddr + 32) & 0x01FFFFFFu), &bgcolor, 8);
            }
            if ((sp & 0x01FFFFFFu) + 0x90 <= 0x02000000u) {
                uint64_t raVal = 0, s0Val = 0, s1Val = 0, s2Val = 0, s3Val = 0, s4Val = 0, s5Val = 0, s6Val = 0;
                std::memcpy(&s0Val, rdram + ((sp + 0x10) & 0x01FFFFFFu), 8);
                std::memcpy(&s1Val, rdram + ((sp + 0x20) & 0x01FFFFFFu), 8);
                std::memcpy(&s2Val, rdram + ((sp + 0x30) & 0x01FFFFFFu), 8);
                std::memcpy(&s3Val, rdram + ((sp + 0x40) & 0x01FFFFFFu), 8);
                std::memcpy(&s4Val, rdram + ((sp + 0x50) & 0x01FFFFFFu), 8);
                std::memcpy(&s5Val, rdram + ((sp + 0x60) & 0x01FFFFFFu), 8);
                std::memcpy(&s6Val, rdram + ((sp + 0x70) & 0x01FFFFFFu), 8);
                std::memcpy(&raVal, rdram + ((sp + 0x80) & 0x01FFFFFFu), 8);
                ctx->r[16] = _mm_cvtsi64_si128(s0Val);
                ctx->r[17] = _mm_cvtsi64_si128(s1Val);
                ctx->r[18] = _mm_cvtsi64_si128(s2Val);
                ctx->r[19] = _mm_cvtsi64_si128(s3Val);
                ctx->r[20] = _mm_cvtsi64_si128(s4Val);
                ctx->r[21] = _mm_cvtsi64_si128(s5Val);
                ctx->r[22] = _mm_cvtsi64_si128(s6Val);
                ctx->r[31] = _mm_cvtsi64_si128(raVal);
            }
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x90);
            ctx->r[2] = _mm_cvtsi32_si128(envAddr);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d0e5cu, fallback4d0e5c);

        // 0x004d1230: sceGszbufaddr(psm in $a0, w in $a1, h in $a2) -> returns zbuf in $v0
        static auto gszbufaddrStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            ps2_stubs::sceGszbufaddr(rdram, ctx, runtime);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d1230u, gszbufaddrStub);

        // 0x004d1278: Fallback continuation of sceGszbufaddr
        static auto fallback4d1278 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            const uint32_t sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));
            ps2_stubs::sceGszbufaddr(rdram, ctx, runtime);
            if ((sp & 0x01FFFFFFu) + 0x40 <= 0x02000000u) {
                uint64_t s0Val = 0, s1Val = 0, s2Val = 0, raVal = 0;
                std::memcpy(&s0Val, rdram + ((sp + 0x00) & 0x01FFFFFFu), 8);
                std::memcpy(&s1Val, rdram + ((sp + 0x10) & 0x01FFFFFFu), 8);
                std::memcpy(&s2Val, rdram + ((sp + 0x20) & 0x01FFFFFFu), 8);
                std::memcpy(&raVal, rdram + ((sp + 0x30) & 0x01FFFFFFu), 8);
                ctx->r[16] = _mm_cvtsi64_si128(s0Val);
                ctx->r[17] = _mm_cvtsi64_si128(s1Val);
                ctx->r[18] = _mm_cvtsi64_si128(s2Val);
                ctx->r[31] = _mm_cvtsi64_si128(raVal);
            }
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x40);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d1278u, fallback4d1278);

        // 0x004d14d4: FUN_004d12f8 (sceGsSetDefDrawEnv) epilogue -> jr $ra; addiu $sp, $sp, 0x70
        static auto returnStub4d14d4 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x70);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d14d4u, returnStub4d14d4);

        // 0x004d27a8: FUN_004d25d0 (sceGsSetDefClear) epilogue -> jr $ra; addiu $sp, $sp, 0x70
        static auto returnStub4d27a8 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x70);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d27a8u, returnStub4d27a8);

        // =========================================================================
        // Global Static C++ Constructors (0x009f1880 .. 0x009f2080)
        // Table in .ctors [0x009f2980, 0x009f29b8) contains 14 function pointers.
        // =========================================================================

        // 1. 0x009f1880: Flag at -0x3f0($gp)
        static auto ctorStub9f1880 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t gp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[28]));
            const uint32_t addr = (gp - 0x3f0u) & 0x01FFFFFFu;
            if (rdram[addr] == 0) {
                rdram[addr] = 1;
            }
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x009f1880u, ctorStub9f1880);

        // 2. 0x009f18a0: Flags at -0x3ec($gp), -0x3e8($gp), -0x3e4($gp)
        static auto ctorStub9f18a0 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t gp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[28]));
            const uint32_t a1 = (gp - 0x3ecu) & 0x01FFFFFFu;
            const uint32_t a2 = (gp - 0x3e8u) & 0x01FFFFFFu;
            const uint32_t a3 = (gp - 0x3e4u) & 0x01FFFFFFu;
            if (rdram[a1] == 0) rdram[a1] = 1;
            if (rdram[a2] == 0) rdram[a2] = 1;
            if (rdram[a3] == 0) rdram[a3] = 1;
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x009f18a0u, ctorStub9f18a0);

        // 3. 0x009f18e0: Calls FUN_00513830 and FUN_004f2180 (__cxa_atexit)
        static auto ctorStub9f18e0 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            *reinterpret_cast<uint32_t*>(rdram + (0x00a32098u & 0x01FFFFFFu)) = 0;
            *reinterpret_cast<uint32_t*>(rdram + (0x00a3209cu & 0x01FFFFFFu)) = 0x00a3209cu;
            *reinterpret_cast<uint32_t*>(rdram + (0x00a320a0u & 0x01FFFFFFu)) = 0x00a3209cu;

            uint32_t head = *reinterpret_cast<uint32_t*>(rdram + (0x00a31fd0u & 0x01FFFFFFu));
            *reinterpret_cast<uint32_t*>(rdram + (0x00a32088u & 0x01FFFFFFu)) = head;
            *reinterpret_cast<uint32_t*>(rdram + (0x00a3208cu & 0x01FFFFFFu)) = 0x00513850u;
            *reinterpret_cast<uint32_t*>(rdram + (0x00a32090u & 0x01FFFFFFu)) = 0x00a32098u;
            *reinterpret_cast<uint32_t*>(rdram + (0x00a31fd0u & 0x01FFFFFFu)) = 0x00a32088u;
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x009f18e0u, ctorStub9f18e0);

        // 4. 0x009f1930: Array ctor with empty constructor 0x005afd90
        static auto ctorStub9f1930 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->r[2] = _mm_cvtsi32_si128(0x00a42a50u);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x009f1930u, ctorStub9f1930);

        // 5. 0x009f1950: Copies 3 words from $gp into 0x009e86f0
        static auto ctorStub9f1950 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            executeMipsStraightLine(rdram, ctx, runtime, 0x009f1950u);
        };
        runtime.replaceFunction(0x009f1950u, ctorStub9f1950);

        // 6. 0x009f1980: Straight-line float matrix/table constants initialization
        static auto ctorStub9f1980 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            executeMipsStraightLine(rdram, ctx, runtime, 0x009f1980u);
        };
        runtime.replaceFunction(0x009f1980u, ctorStub9f1980);

        // 7. 0x009f1b60: Straight-line float matrix/table constants initialization
        static auto ctorStub9f1b60 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            executeMipsStraightLine(rdram, ctx, runtime, 0x009f1b60u);
        };
        runtime.replaceFunction(0x009f1b60u, ctorStub9f1b60);

        // 8. 0x009f1d40: Sets up 4x4 identity matrix at 0x009fd770
        static auto ctorStub9f1d40 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            executeMipsStraightLine(rdram, ctx, runtime, 0x009f1d40u);
        };
        runtime.replaceFunction(0x009f1d40u, ctorStub9f1d40);

        // 9. 0x009f1dd0: Sets up constants at 0x009fd7b0
        static auto ctorStub9f1dd0 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            executeMipsStraightLine(rdram, ctx, runtime, 0x009f1dd0u);
        };
        runtime.replaceFunction(0x009f1dd0u, ctorStub9f1dd0);

        // 10. 0x009f1e10: Straight-line float table constants initialization
        static auto ctorStub9f1e10 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            executeMipsStraightLine(rdram, ctx, runtime, 0x009f1e10u);
        };
        runtime.replaceFunction(0x009f1e10u, ctorStub9f1e10);

        // 11. 0x009f1ff0: Copies word from $gp to 0x00970ee0
        static auto ctorStub9f1ff0 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            executeMipsStraightLine(rdram, ctx, runtime, 0x009f1ff0u);
        };
        runtime.replaceFunction(0x009f1ff0u, ctorStub9f1ff0);

        // 12. 0x009f2000: Array ctors (runs 12, then 13 and 14, then terminates ctors loop)
        // Notice: 0x9f2060 and 0x9f2080 are past g_ps2RecompiledFunctionTableEnd (0x9f2004).
        // Executing them directly here and advancing s0 to s1 ensures all 14 ctors finish cleanly.
        static auto ctorStub9f2000 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            // Ctors 13: 0x009f2060 (writes (0, 0, 1.0f, 0) at 0x00a93260)
            executeMipsStraightLine(rdram, ctx, runtime, 0x009f2060u);

            // Ctors 14: 0x009f2080 (copies float constants from $gp into 0x00995520)
            executeMipsStraightLine(rdram, ctx, runtime, 0x009f2080u);

            // In FUN_004f21b0 ctors loop: s0 is ctx->r[16], s1 is ctx->r[17].
            // Setting s0 = s1 ensures the loop terminates on next check.
            ctx->r[16] = ctx->r[17];
            // Safeguard: also patch table entries 13 and 14 in RDRAM to point to 0x009f2000
            *reinterpret_cast<uint32_t *>(rdram + (0x009f29b0u & 0x01FFFFFFu)) = 0x009f2000u;
            *reinterpret_cast<uint32_t *>(rdram + (0x009f29b4u & 0x01FFFFFFu)) = 0x009f2000u;
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x009f2000u, ctorStub9f2000);

        // 0x004c2bd0: Continuation and epilogue of FUN_004c2b40 (aligned heap allocation)
        static auto continuation4c2bd0 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));
            const uint32_t s0 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[16])); // alignment
            const uint32_t s1 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[17])); // alloc ptr from func_4C3000

            if (s1 == 0u) {
                ctx->r[2] = _mm_setzero_si128();
            } else {
                uint32_t res = s1;
                if (s0 > 0u && (res % s0) != 0u) {
                    res = (res + s0 - 1u) & ~(s0 - 1u);
                }
                ctx->r[2] = _mm_cvtsi32_si128(res);
            }

            // Epilogue of FUN_004c2b40: restore saved registers from sp (frame size 0x30)
            if ((sp & 0x01FFFFFFu) + 0x30 <= 0x02000000u) {
                uint64_t r_s0 = 0, r_s1 = 0, r_s2 = 0, r_s3 = 0, r_s4 = 0, r_ra = 0;
                std::memcpy(&r_s0, rdram + ((sp + 0x00) & 0x01FFFFFFu), 8);
                std::memcpy(&r_s1, rdram + ((sp + 0x08) & 0x01FFFFFFu), 8);
                std::memcpy(&r_s2, rdram + ((sp + 0x10) & 0x01FFFFFFu), 8);
                std::memcpy(&r_s3, rdram + ((sp + 0x18) & 0x01FFFFFFu), 8);
                std::memcpy(&r_s4, rdram + ((sp + 0x20) & 0x01FFFFFFu), 8);
                std::memcpy(&r_ra, rdram + ((sp + 0x28) & 0x01FFFFFFu), 8);
                ctx->r[16] = _mm_cvtsi64_si128(r_s0);
                ctx->r[17] = _mm_cvtsi64_si128(r_s1);
                ctx->r[18] = _mm_cvtsi64_si128(r_s2);
                ctx->r[19] = _mm_cvtsi64_si128(r_s3);
                ctx->r[20] = _mm_cvtsi64_si128(r_s4);
                ctx->r[31] = _mm_cvtsi64_si128(r_ra);
            }
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x30);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004c2bd0u, continuation4c2bd0);

        // 0x004c4d08: Standard C library qsort(base, nmemb, size, compar)
        static auto qsortOverride = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            const uint32_t base = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            const uint32_t nmemb = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
            const uint32_t size = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[6]));
            const uint32_t compar = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[7]));
            const uint32_t returnPc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));

            if (nmemb <= 1u || size == 0u || base == 0u || compar == 0u || !runtime) {
                ctx->pc = returnPc;
                return;
            }

            const uint64_t totalBytes = static_cast<uint64_t>(nmemb) * size;
            const uint32_t physBase = base & 0x01FFFFFFu;
            if (physBase + totalBytes > 0x02000000u || nmemb > 1000000u) {
                ctx->pc = returnPc;
                return;
            }

            PS2Runtime::RecompiledFunction compFn = runtime->lookupFunction(compar);
            if (!compFn) {
                ctx->pc = returnPc;
                return;
            }

            auto compareElements = [&](uint32_t addrA, uint32_t addrB) -> int {
                R5900Context savedCtx = *ctx;
                ctx->r[4] = _mm_cvtsi32_si128(addrA);
                ctx->r[5] = _mm_cvtsi32_si128(addrB);
                ctx->r[31] = _mm_cvtsi32_si128(0x004c0008u); // safe return address
                ctx->pc = compar;

                compFn(rdram, ctx, runtime);

                const int res = static_cast<int>(_mm_cvtsi128_si32(ctx->r[2]));
                *ctx = savedCtx;
                return res;
            };

            std::vector<uint32_t> indices(nmemb);
            for (uint32_t i = 0; i < nmemb; ++i) {
                indices[i] = i;
            }

            std::stable_sort(indices.begin(), indices.end(), [&](uint32_t idxA, uint32_t idxB) {
                const uint32_t addrA = base + idxA * size;
                const uint32_t addrB = base + idxB * size;
                return compareElements(addrA, addrB) < 0;
            });

            std::vector<uint8_t> temp(totalBytes);
            std::memcpy(temp.data(), rdram + physBase, totalBytes);
            for (uint32_t i = 0; i < nmemb; ++i) {
                const uint32_t srcIdx = indices[i];
                std::memcpy(rdram + physBase + i * size, temp.data() + srcIdx * size, size);
            }

            ctx->pc = returnPc;
        };
        runtime.replaceFunction(0x004c4d08u, qsortOverride);

        // Fallbacks in case code directly reaches internal addresses of qsort
        static auto qsortReturnFallback = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004c4e78u, qsortReturnFallback);
        runtime.replaceFunction(0x004c4e84u, qsortReturnFallback);

        // Standard C math library overrides
        // 0x004ce340: sinf(float x in $f12) -> float in $f0
        static auto sinfOverride = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            const float x = ctx->f[12];
            ctx->f[0] = std::sin(x);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004ce340u, sinfOverride);

        // 0x004ce168: cosf(float x in $f12) -> float in $f0
        static auto cosfOverride = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            const float x = ctx->f[12];
            ctx->f[0] = std::cos(x);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004ce168u, cosfOverride);

        // 0x004ce240: fabsf(float x in $f12) -> float in $f0
        static auto fabsfOverride = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            const float x = ctx->f[12];
            ctx->f[0] = std::fabs(x);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004ce240u, fabsfOverride);

        // Fallback for truncated math helper at 0x004cdb78
        static auto mathHelperFallback = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004cdb78u, mathHelperFallback);

        // 0x004d801c: Epilogue of FUN_004d8000
        static auto epilogue4d801c = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));
            if ((sp & 0x01FFFFFFu) + 8 <= 0x02000000u) {
                uint64_t ra = 0;
                std::memcpy(&ra, rdram + (sp & 0x01FFFFFFu), 8);
                ctx->r[31] = _mm_cvtsi64_si128(ra);
            }
            ctx->r[2] = _mm_setzero_si128(); // move $v0, $zero
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x10);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d801cu, epilogue4d801c);

        // 0x007afbf8: Initialize rom0:ROMVER buffer
        static auto romverInitOverride = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t buf = 0x0096DC38u;
            const uint32_t phys = buf & 0x01FFFFFFu;
            if (phys + 16 <= 0x02000000u) {
                // "0190EC20040510\n" (PAL Europe retail console ROMVER)
                static const char romverStr[16] = "0190EC20040510\n";
                std::memcpy(rdram + phys, romverStr, 16);
            }
            ctx->r[2] = _mm_cvtsi32_si128(buf);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x007afbf8u, romverInitOverride);

        // 0x004d8684: Continuation of FUN_004d8628 (sceOpen fallback)
        static auto continuation4d8684 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));
            if ((sp & 0x01FFFFFFu) + 0x150 <= 0x02000000u) {
                uint64_t r_ra = 0, r_s0 = 0, r_s1 = 0, r_s2 = 0, r_s3 = 0, r_s4 = 0, r_s5 = 0, r_s6 = 0, r_s7 = 0, r_fp = 0;
                std::memcpy(&r_s0, rdram + ((sp + 0x40) & 0x01FFFFFFu), 8);
                std::memcpy(&r_s1, rdram + ((sp + 0x50) & 0x01FFFFFFu), 8);
                std::memcpy(&r_s2, rdram + ((sp + 0x60) & 0x01FFFFFFu), 8);
                std::memcpy(&r_s3, rdram + ((sp + 0x70) & 0x01FFFFFFu), 8);
                std::memcpy(&r_s4, rdram + ((sp + 0x80) & 0x01FFFFFFu), 8);
                std::memcpy(&r_s5, rdram + ((sp + 0x90) & 0x01FFFFFFu), 8);
                std::memcpy(&r_s6, rdram + ((sp + 0xa0) & 0x01FFFFFFu), 8);
                std::memcpy(&r_s7, rdram + ((sp + 0xb0) & 0x01FFFFFFu), 8);
                std::memcpy(&r_fp, rdram + ((sp + 0xc0) & 0x01FFFFFFu), 8);
                std::memcpy(&r_ra, rdram + ((sp + 0xd0) & 0x01FFFFFFu), 8);
                ctx->r[16] = _mm_cvtsi64_si128(r_s0);
                ctx->r[17] = _mm_cvtsi64_si128(r_s1);
                ctx->r[18] = _mm_cvtsi64_si128(r_s2);
                ctx->r[19] = _mm_cvtsi64_si128(r_s3);
                ctx->r[20] = _mm_cvtsi64_si128(r_s4);
                ctx->r[21] = _mm_cvtsi64_si128(r_s5);
                ctx->r[22] = _mm_cvtsi64_si128(r_s6);
                ctx->r[23] = _mm_cvtsi64_si128(r_s7);
                ctx->r[30] = _mm_cvtsi64_si128(r_fp);
                ctx->r[31] = _mm_cvtsi64_si128(r_ra);
            }
            ctx->r[2] = _mm_cvtsi32_si128(-1); // return -1 (error/not found)
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x150);
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d8684u, continuation4d8684);

        // 0x0050f750: Task/Scene update hook - monitors frame count and captures presentation screenshots
        static auto frameHook50f750 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            static uint32_t s_frameCounter = 0;
            s_frameCounter++;

            // Charge realistic execution cycles for the task update (10,000 cycles)
            // so m_eeCycle advances and triggers scheduled VBlank / VSync interrupts at 60 Hz.
            if (runtime) {
                runtime->eeCheckpointDue(10000u);
            }

            if (s_frameCounter == 1 || s_frameCounter == 10 || s_frameCounter == 30 || s_frameCounter == 60 || s_frameCounter == 120 ||
                s_frameCounter == 300 || (s_frameCounter % 600) == 0) {
                const std::string filename = "/home/lubonch/repos/Sword-Of-Etheria-PC-Recomp/work/build-game/frame_" + std::to_string(s_frameCounter) + ".bmp";
                savePresentationBmp(runtime, filename.c_str());
            }

            ctx->pc = 0x0050f750u;
            // 2026-10-11: delegar en el ORIGINAL de 0x50f750 (antes corria el stub
            // FUN_004eb8d0, workaround de cuando las megas tenian el End inflado).
            if (g_orig50f750) {
                g_orig50f750(rdram, ctx, runtime);
            }
        };
        g_orig50f750 = runtime.lookupFunction(0x0050f750u);
        runtime.replaceFunction(0x0050f750u, frameHook50f750);

        // Container / iterator inline helpers (0x004f4b50 - 0x004f4be0)
        // 0x004f4b50: sw $a1, 0($a0); jr $ra; daddu $v0, $a0, $zero
        static auto helper4f4b50 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t a0 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            const uint32_t a1 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
            if ((a0 & 0x01FFFFFFu) + 4 <= 0x02000000u) {
                std::memcpy(rdram + (a0 & 0x01FFFFFFu), &a1, 4);
            }
            ctx->r[2] = ctx->r[4]; // move $v0, $a0
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004f4b50u, helper4f4b50);

        // 0x004f4b60 / 0x004f4bb0 / 0x004f4bc0: jr $ra; addiu $v0, $a0, 4
        static auto helperAdvance4 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            const uint32_t a0 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            ctx->r[2] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<int32_t>(a0 + 4u)));
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004f4b60u, helperAdvance4);
        runtime.replaceFunction(0x004f4bb0u, helperAdvance4);
        runtime.replaceFunction(0x004f4bc0u, helperAdvance4);

        // 0x004f4b70: addiu $v0, $a1, 4; sw $v0, 0($a0); jr $ra; daddu $v0, $a0, $zero
        static auto helper4f4b70 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t a0 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            const uint32_t a1 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
            const uint32_t val = a1 + 4u;
            if ((a0 & 0x01FFFFFFu) + 4 <= 0x02000000u) {
                std::memcpy(rdram + (a0 & 0x01FFFFFFu), &val, 4);
            }
            ctx->r[2] = ctx->r[4]; // move $v0, $a0
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004f4b70u, helper4f4b70);

        // 0x004f4b80: lw $v1, 0($a1); daddu $v0, $a0, $zero; jr $ra; sw $v1, 0($a0)
        static auto helper4f4b80 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t a0 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            const uint32_t a1 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
            uint32_t val = 0;
            if ((a1 & 0x01FFFFFFu) + 4 <= 0x02000000u) {
                std::memcpy(&val, rdram + (a1 & 0x01FFFFFFu), 4);
            }
            if ((a0 & 0x01FFFFFFu) + 4 <= 0x02000000u) {
                std::memcpy(rdram + (a0 & 0x01FFFFFFu), &val, 4);
            }
            ctx->r[2] = ctx->r[4]; // move $v0, $a0
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004f4b80u, helper4f4b80);

        // 0x004f4b90: lw $v1, 8($a1); daddu $v0, $a0, $zero; jr $ra; sw $v1, 0($a0)
        static auto helper4f4b90 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            const uint32_t a0 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            const uint32_t a1 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
            uint32_t val = 0;
            if ((a1 & 0x01FFFFFFu) + 12 <= 0x02000000u) {
                std::memcpy(&val, rdram + ((a1 + 8) & 0x01FFFFFFu), 4);
            }
            if ((a0 & 0x01FFFFFFu) + 4 <= 0x02000000u) {
                std::memcpy(rdram + (a0 & 0x01FFFFFFu), &val, 4);
            }
            ctx->r[2] = ctx->r[4]; // move $v0, $a0
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004f4b90u, helper4f4b90);

        // 0x004f4ba0 / 0x004f4bd0 / 0x004f4be0: jr $ra; daddu $v0, $a0, $zero
        static auto helperReturnA0 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->r[2] = ctx->r[4]; // move $v0, $a0
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004f4ba0u, helperReturnA0);
        runtime.replaceFunction(0x004f4bd0u, helperReturnA0);
        runtime.replaceFunction(0x004f4be0u, helperReturnA0);

        // 0x004c1970 / 0x004c1978: main (FUN_004c1900) continuation and game loop dispatch
        static auto mainGameLoopDispatch = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            uint32_t s4 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[20]));
            if (s4 == 0) s4 = 0x00870000u;
            uint32_t a0Val = 0;
            std::memcpy(&a0Val, rdram + ((s4 + 0x1bf0) & 0x01FFFFFFu), 4);
            ctx->r[4] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<int32_t>(a0Val)));

            uint32_t loopFn = 0;
            if (a0Val != 0) {
                std::memcpy(&loopFn, rdram + ((a0Val + 0x3c) & 0x01FFFFFFu), 4);
            }
            ctx->r[2] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<int32_t>(loopFn)));

            RUNTIME_LOG("[game_overrides] main loop dispatch: a0=0x" << std::hex << a0Val
                        << " loopFn=0x" << loopFn << std::dec);

            if (loopFn != 0) {
                ctx->r[31] = _mm_cvtsi64_si128(0x004c1988u);
                ctx->pc = loopFn;
                if (!runtime->dispatchGuestBranch(rdram, ctx, loopFn, 0x004c1980u, 0x004c1988u, PS2Runtime::GuestBranchKind::IndirectCall, "JALR")) {
                    return;
                }
            }

            // Epilogue of main (0x4c198c - 0x4c19a8):
            const uint32_t sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29]));
            if ((sp & 0x01FFFFFFu) + 0x30 <= 0x02000000u) {
                uint64_t s0Val = 0, s1Val = 0, s2Val = 0, s3Val = 0, s4Val = 0, raVal = 0;
                std::memcpy(&s0Val, rdram + ((sp + 0x00) & 0x01FFFFFFu), 8);
                std::memcpy(&s1Val, rdram + ((sp + 0x08) & 0x01FFFFFFu), 8);
                std::memcpy(&s2Val, rdram + ((sp + 0x10) & 0x01FFFFFFu), 8);
                std::memcpy(&s3Val, rdram + ((sp + 0x18) & 0x01FFFFFFu), 8);
                std::memcpy(&s4Val, rdram + ((sp + 0x20) & 0x01FFFFFFu), 8);
                std::memcpy(&raVal, rdram + ((sp + 0x28) & 0x01FFFFFFu), 8);
                ctx->r[16] = _mm_cvtsi64_si128(s0Val);
                ctx->r[17] = _mm_cvtsi64_si128(s1Val);
                ctx->r[18] = _mm_cvtsi64_si128(s2Val);
                ctx->r[19] = _mm_cvtsi64_si128(s3Val);
                ctx->r[20] = _mm_cvtsi64_si128(s4Val);
                ctx->r[31] = _mm_cvtsi64_si128(raVal);
            }
            ctx->r[29] = _mm_cvtsi64_si128(_mm_cvtsi128_si64(ctx->r[29]) + 0x30);
            ctx->pc = 0x004c0218u;
            (void)runtime->dispatchGuestBranch(rdram, ctx, 0x004c0218u, 0x004c19a4u, 0x004c0218u, PS2Runtime::GuestBranchKind::DirectJump, "J");
        };
        runtime.replaceFunction(0x004c1970u, mainGameLoopDispatch);
        runtime.replaceFunction(0x004c1978u, mainGameLoopDispatch);

        // 0x004d3af0 - 0x004d3d80: Sony SDK Cache Management (SyncDCache, InvalidateDCache, FlushCache)
        // Cache instructions are no-ops on x86-64 unified memory architectures.
        static auto cacheNoOpStub = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)rdram; (void)runtime;
            ctx->r[2] = _mm_setzero_si128(); // return 0
            ctx->pc = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        };
        runtime.replaceFunction(0x004d3af0u, cacheNoOpStub);
        runtime.replaceFunction(0x004d3b04u, cacheNoOpStub);
        runtime.replaceFunction(0x004d3b98u, cacheNoOpStub);
        runtime.replaceFunction(0x004d3c30u, cacheNoOpStub);
        runtime.replaceFunction(0x004d3c44u, cacheNoOpStub);
        runtime.replaceFunction(0x004d3cd8u, cacheNoOpStub);
        runtime.replaceFunction(0x004d3d58u, cacheNoOpStub);
        runtime.replaceFunction(0x004d3d80u, cacheNoOpStub);
        // 0x0065f820: Truncated init block, dispatch directly to 0x65f838 to continue execution.
        static auto stub65f820 = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
            (void)runtime;
            uint32_t sp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[29])) - 0x50;
            ctx->r[29] = _mm_cvtsi32_si128(sp);
            ctx->r[2] = _mm_cvtsi32_si128(0x460000);
            uint64_t ra = static_cast<uint64_t>(_mm_cvtsi128_si64(ctx->r[31]));
            std::memcpy(rdram + ((sp + 0x40) & 0x1FFFFFFF), &ra, sizeof(ra));
            ctx->r[5] = _mm_cvtsi32_si128(1);
            alignas(16) uint8_t s3[16];
            _mm_store_si128(reinterpret_cast<__m128i*>(s3), ctx->r[19]);
            std::memcpy(rdram + ((sp + 0x30) & 0x1FFFFFFF), s3, 16);
            ctx->r[6] = _mm_cvtsi32_si128(0xF108);
            ctx->pc = 0x65f838u;
        };
        runtime.replaceFunction(0x65f820u, stub65f820);
    }
}

PS2_REGISTER_GAME_OVERRIDE(
    "SwordOfEtheria_SyscallStubs",
    "SLES_537.68",
    0x004C0008u,
    0u,
    &applySwordOfEtheriaOverrides
)
