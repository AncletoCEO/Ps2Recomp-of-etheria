#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/ps2_gs_common.h"
#include "runtime/gs/ps2_gs_psmct16.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/gs/ps2_gs_psmt4.h"
#include "runtime/gs/ps2_gs_psmt8.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "ps2_log.h"
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unordered_map>

using namespace GSInternal;

// 2026-10-19 (hito 1.2): definidos en game_overrides.cpp; callback/widget de UI en ejecucion.
extern uint32_t g_swordUiCbTag;
extern uint32_t g_swordUiCurWidget;

namespace
{
    float fabsQ(float q)
    {
        return (std::fabs(q) > 1.0e-8f) ? q : 1.0f;
    }

    u16 Rgba8888ToRgba5551(u32 c)
    {
        uint32_t r = ((c >> 0) & 0xFF) >> 3;
        uint32_t g = ((c >> 8) & 0xFF) >> 3;
        uint32_t b = ((c >> 16) & 0xFF) >> 3;
        uint32_t a = ((c >> 24) & 0xFF) >> 7;

        return (r | (g << 5) | (b << 10) | (a << 15));
    }

    u32 Rgba5551ToRgba8888(u16 c)
    {
        u32 r = ((c >> 0) & 0x1F) << 3;
        u32 g = ((c >> 5) & 0x1F) << 3;
        u32 b = ((c >> 10) & 0x1F) << 3;
        u32 a = ((c >> 15) & 0x01) << 7;

        return (r | (g << 8) | (b << 16) | (a << 24));
    }

    u32 pack32(u8 r, u8 g, u8 b, u8 a)
    {
        return static_cast<u32>(r) | (g << 8) | (b << 16) | (a << 24);
    }

    uint32_t applyTexa(const GSTexaReg &texa, uint8_t psm, uint32_t texel)
    {
        if (psm == GS_PSM_CT32)
            return texel;

        const uint8_t r = static_cast<uint8_t>(texel & 0xFFu);
        const uint8_t g = static_cast<uint8_t>((texel >> 8) & 0xFFu);
        const uint8_t b = static_cast<uint8_t>((texel >> 16) & 0xFFu);
        const bool rgbZero = r == 0u && g == 0u && b == 0u;
        uint8_t a = static_cast<uint8_t>((texel >> 24) & 0xFFu);

        switch (psm)
        {
        case GS_PSM_CT24:
            a = (texa.aem && rgbZero) ? 0u : texa.ta0;
            break;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            if ((a & 0x80u) != 0u)
                a = texa.ta1;
            else
                a = (texa.aem && rgbZero) ? 0u : texa.ta0;
            break;
        default:
            break;
        }

        return (texel & 0x00FFFFFFu) | (static_cast<uint32_t>(a) << 24);
    }

    uint32_t addrPSMCT16Family(uint32_t basePtr, uint32_t width, uint8_t psm, uint32_t x, uint32_t y)
    {
        switch (psm)
        {
        case GS_PSM_CT16:
            return GSPSMCT16::addrPSMCT16(basePtr, width, x, y);
        case GS_PSM_CT16S:
            return GSPSMCT16::addrPSMCT16S(basePtr, width, x, y);
        case GS_PSM_Z16:
            return GSPSMCT16::addrPSMZ16(basePtr, width, x, y);
        case GS_PSM_Z16S:
            return GSPSMCT16::addrPSMZ16S(basePtr, width, x, y);
        default:
            return 0u;
        }
    }

    std::atomic<uint32_t> s_debugPrimitiveCount{0};
    std::atomic<uint32_t> s_debugPixelCount{0};
    std::atomic<uint32_t> s_debugContext1PrimitiveCount{0};
    std::atomic<uint32_t> s_debugFbp150PixelCount{0};

    int wrapTextureCoordinate(int coordinate,
                              int textureSize,
                              uint8_t mode,
                              uint16_t regionMin,
                              uint16_t regionMax)
    {
        switch (mode & 0x3u)
        {
        case 0: // REPEAT
            return static_cast<int>(static_cast<uint32_t>(coordinate) & static_cast<uint32_t>(textureSize - 1));
        case 1: // CLAMP
            return clampInt(coordinate, 0, textureSize - 1);
        case 2: // REGION_CLAMP
            return std::min(std::max(coordinate, static_cast<int>(regionMin)), static_cast<int>(regionMax));
        case 3: // REGION_REPEAT
            return static_cast<int>((static_cast<uint32_t>(coordinate) & static_cast<uint32_t>(regionMin)) | static_cast<uint32_t>(regionMax));
        default:
            return coordinate;
        }
    }

    bool passesAlphaTest(uint64_t testReg, uint8_t alpha)
    {
        // DIAG 2026-10-18 (Gemini v71 paso 2): bypass del alpha test para probar que la geometria
        // escribe en VRAM si no se descarta. Env-gated, default OFF.
        if (std::getenv("SWORD_NO_ALPHA_TEST") != nullptr)
            return true;
        if ((testReg & 0x1u) == 0u)
            return true;

        const uint8_t atst = static_cast<uint8_t>((testReg >> 1) & 0x7u);
        const uint8_t aref = static_cast<uint8_t>((testReg >> 4) & 0xFFu);

        switch (atst)
        {
        case 0:
            return false;
        case 1:
            return true;
        case 2:
            return alpha < aref;
        case 3:
            return alpha <= aref;
        case 4:
            return alpha == aref;
        case 5:
            return alpha >= aref;
        case 6:
            return alpha > aref;
        case 7:
            return alpha != aref;
        default:
            return true;
        }
    }

    struct PixelWriteMask
    {
        bool writeRgb = true;
        bool writeAlpha = true;
        bool writeDepth = true;

        bool writesFramebuffer() const
        {
            return writeRgb || writeAlpha;
        }

        bool writesAnything() const
        {
            return writesFramebuffer() || writeDepth;
        }
    };

    PixelWriteMask classifyAlphaTest(uint64_t testReg, uint8_t alpha, uint8_t framePsm)
    {
        const bool pass = passesAlphaTest(testReg, alpha);
        if (pass)
            return {};

        // TEST.AFAIL controls what happens when the alpha comparison fails.
        switch (static_cast<uint8_t>((testReg >> 12) & 0x3u))
        {
        case 1: // FB_ONLY
            return {true, true, false};
        case 2: // ZB_ONLY
            return {false, false, true};
        case 3: // RGB_ONLY
            // RGB_ONLY is only distinct for RGBA32. The GS treats it as
            // FB_ONLY for RGB24 and RGBA16 framebuffers.
            if (framePsm == GS_PSM_CT32)
                return {true, false, false};
            return {true, true, false};
        case 0: // KEEP
        default:
            return {false, false, false};
        }
    }

    bool passesDestinationAlphaTest(uint64_t testReg, uint8_t framePsm, uint32_t rawFramebufferPixel)
    {
        const bool date = ((testReg >> 14) & 0x1u) != 0u;
        if (!date)
            return true;

        const bool datm = ((testReg >> 15) & 0x1u) != 0u;
        switch (framePsm)
        {
        case GS_PSM_CT32:
            return (((rawFramebufferPixel >> 31) & 0x1u) != 0u) == datm;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            return (((rawFramebufferPixel >> 15) & 0x1u) != 0u) == datm;
        case GS_PSM_CT24:
            // RGB24 has no destination alpha, so DATE always passes.
            return true;
        default:
            return true;
        }
    }

    struct TextureCombineResult
    {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a;
    };

    TextureCombineResult combineTexture(const GSTex0Reg &tex,
                                        uint8_t vr,
                                        uint8_t vg,
                                        uint8_t vb,
                                        uint8_t va,
                                        uint8_t tr,
                                        uint8_t tg,
                                        uint8_t tb,
                                        uint8_t ta)
    {
        // DIAG 2026-10-18 (Gemini v77): si el vertice viene RGB=0, MODULATE anula el texel.
        // Con SWORD_FORCE_VTX_RGB=1 se usa 1.0 (128) para los draws de UI (tbp0 >= 10240).
        if (std::getenv("SWORD_FORCE_VTX_RGB") != nullptr && tex.tbp0 >= 10240u &&
            vr == 0u && vg == 0u && vb == 0u)
        {
            vr = 128u;
            vg = 128u;
            vb = 128u;
        }
        // DIAG 2026-10-18 (Gemini v79): la CLUT del atlas (cbp=10300) tiene valores 4-bit
        // (A y RGB <= 15). Con SWORD_UI_COLOR_FIX=1 se expanden a escala GS para ver si el texto
        // aparece. NO es fix real.
        if (std::getenv("SWORD_UI_COLOR_FIX") != nullptr && tex.tbp0 >= 10240u)
        {
            if (ta > 0u && ta <= 15u)
                ta = static_cast<uint8_t>((static_cast<uint32_t>(ta) * 128u + 7u) / 15u);
            if (tr <= 15u)
                tr = static_cast<uint8_t>(tr * 17u);
            if (tg <= 15u)
                tg = static_cast<uint8_t>(tg * 17u);
            if (tb <= 15u)
                tb = static_cast<uint8_t>(tb * 17u);
            if (vr == 0u && vg == 0u && vb == 0u)
            {
                vr = 128u;
                vg = 128u;
                vb = 128u;
            }
            if (va == 0u)
                va = 128u;
        }
        const bool textureHasAlpha = tex.tcc != 0u;
        TextureCombineResult out{tr, tg, tb, textureHasAlpha ? ta : va};

        switch (tex.tfx)
        {
        case 0: // MODULATE
            out.r = clampU8((tr * vr) >> 7);
            out.g = clampU8((tg * vg) >> 7);
            out.b = clampU8((tb * vb) >> 7);
            out.a = textureHasAlpha ? clampU8((ta * va) >> 7) : va;
            break;
        case 1: // DECAL
            out.r = tr;
            out.g = tg;
            out.b = tb;
            out.a = textureHasAlpha ? ta : va;
            break;
        case 2: // HIGHLIGHT
            out.r = clampU8(((tr * vr) >> 7) + va);
            out.g = clampU8(((tg * vg) >> 7) + va);
            out.b = clampU8(((tb * vb) >> 7) + va);
            out.a = textureHasAlpha ? clampU8(ta + va) : va;
            break;
        case 3: // HIGHLIGHT2
            out.r = clampU8(((tr * vr) >> 7) + va);
            out.g = clampU8(((tg * vg) >> 7) + va);
            out.b = clampU8(((tb * vb) >> 7) + va);
            out.a = textureHasAlpha ? ta : va;
            break;
        default:
            out.r = tr;
            out.g = tg;
            out.b = tb;
            out.a = textureHasAlpha ? ta : va;
            break;
        }

        return out;
    }

    uint32_t swizzleClutIndexCSM1(uint32_t index)
    {
        // CSM1 swaps address bits 3 and 4. Preserve the remaining bits:
        // 16-bit CLUTs expose a ninth address bit through CSA[4].
        return (index & ~0x18u) | ((index & 0x08u) << 1u) | ((index & 0x10u) >> 1u);
    }

    bool isFourBitIndexedPsm(uint8_t psm)
    {
        return psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
    }

    bool isEightBitIndexedPsm(uint8_t psm)
    {
        return psm == GS_PSM_T8 || psm == GS_PSM_T8H;
    }

    uint8_t lerpChannel(uint8_t c00, uint8_t c10, uint8_t c01, uint8_t c11, float fx, float fy)
    {
        const float top = static_cast<float>(c00) + (static_cast<float>(c10) - static_cast<float>(c00)) * fx;
        const float bottom = static_cast<float>(c01) + (static_cast<float>(c11) - static_cast<float>(c01)) * fx;
        return clampU8(static_cast<int>(std::lround(top + (bottom - top) * fy)));
    }
}

namespace
{
    static constexpr uint32_t kDefaultDisplayWidth = 640u;
    static constexpr uint32_t kDefaultDisplayHeight = 448u;
    static constexpr uint32_t kHostFrameWidth = 640u;
    static constexpr uint32_t kHostFrameHeight = 512u;

    uint16_t encodeFramePixelPSMCT16(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        return static_cast<uint16_t>(((r >> 3) & 0x1Fu) |
                                     (((g >> 3) & 0x1Fu) << 5) |
                                     (((b >> 3) & 0x1Fu) << 10) |
                                     ((a >= 0x40u) ? 0x8000u : 0u));
    }

    void decodeDisplaySize(uint64_t display64, uint32_t &outWidth, uint32_t &outHeight)
    {
        const uint32_t dw = static_cast<uint32_t>((display64 >> 32) & 0x0FFFu);
        const uint32_t dh = static_cast<uint32_t>((display64 >> 44) & 0x07FFu);
        const uint32_t magh = static_cast<uint32_t>((display64 >> 23) & 0x0Fu);

        outWidth = (dw + 1u) / (magh + 1u);
        outHeight = dh + 1u;
        if (outWidth < 64u || outHeight < 64u)
        {
            outWidth = kDefaultDisplayWidth;
            outHeight = kDefaultDisplayHeight;
        }
        outWidth = std::min<uint32_t>(outWidth, kHostFrameWidth);
        outHeight = std::min<uint32_t>(outHeight, kHostFrameHeight);
    }

    GSFrameReg decodeDisplayFrame(uint64_t dispfb64)
    {
        GSFrameReg frame{};
        frame.fbp = static_cast<uint32_t>(dispfb64 & 0x1FFu);
        frame.fbw = static_cast<uint32_t>((dispfb64 >> 9) & 0x3Fu);
        frame.psm = static_cast<uint8_t>((dispfb64 >> 15) & 0x1Fu);
        return frame;
    }

    struct GSDisplayReadOrigin
    {
        uint32_t x = 0u;
        uint32_t y = 0u;
    };

    GSDisplayReadOrigin decodeDisplayReadOrigin(uint64_t dispfb64)
    {
        return {
            static_cast<uint32_t>((dispfb64 >> 32) & 0x7FFu),
            static_cast<uint32_t>((dispfb64 >> 43) & 0x7FFu)};
    }

    bool hasDisplaySetup(uint64_t display64, const GSFrameReg &frame)
    {
        const uint32_t dw = static_cast<uint32_t>((display64 >> 32) & 0x0FFFu);
        const uint32_t dh = static_cast<uint32_t>((display64 >> 44) & 0x07FFu);
        const uint32_t magh = static_cast<uint32_t>((display64 >> 23) & 0x0Fu);
        return frame.fbw != 0u || dw != 0u || dh != 0u || magh != 0u;
    }

    struct GSPmodeState
    {
        bool enableCrt1 = false;
        bool enableCrt2 = false;
        bool mmod = false;
        bool amod = false;
        bool slbg = false;
        uint8_t alp = 0u;
    };

    GSPmodeState decodePmode(uint64_t pmode64)
    {
        return {
            (pmode64 & 0x1ull) != 0ull,
            (pmode64 & 0x2ull) != 0ull,
            ((pmode64 >> 5) & 0x1ull) != 0ull,
            ((pmode64 >> 6) & 0x1ull) != 0ull,
            ((pmode64 >> 7) & 0x1ull) != 0ull,
            static_cast<uint8_t>((pmode64 >> 8) & 0xFFu)};
    }

    struct GSSmode2State
    {
        bool interlaced = false;
        bool frameMode = true;
    };

    GSSmode2State decodeSMode2(uint64_t smode2)
    {
        return {(smode2 & 0x1ull) != 0ull, ((smode2 >> 1) & 0x1ull) != 0ull};
    }

    void applyFieldPresentation(std::vector<uint8_t> &pixels, uint32_t width, uint32_t height, bool oddField)
    {
        if (pixels.empty() || width == 0u || height < 2u)
            return;
        const std::vector<uint8_t> source = pixels;
        for (uint32_t y = 0; y < height; ++y)
        {
            uint32_t sourceY = ((y >> 1u) << 1u) + (oddField ? 1u : 0u);
            if (sourceY >= height)
                sourceY = height - 1u;
            std::memcpy(pixels.data() + y * kHostFrameWidth * 4u,
                        source.data() + sourceY * kHostFrameWidth * 4u,
                        width * 4u);
        }
    }

    void normalizePresentationAlpha(std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
    {
        for (uint32_t y = 0; y < height; ++y)
        {
            uint8_t *row = pixels.data() + y * kHostFrameWidth * 4u;
            for (uint32_t x = 0; x < width; ++x)
                row[x * 4u + 3u] = 255u;
        }
    }

    uint8_t blendPresentationChannel(uint8_t src, uint8_t dst, uint32_t factor)
    {
        const int delta = static_cast<int>(src) - static_cast<int>(dst);
        return GSInternal::clampU8(static_cast<int>(dst) + ((delta * static_cast<int>(factor)) / 255));
    }

    uint32_t countNonBlackPixels(const std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
    {
        uint32_t count = 0u;
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t *row = pixels.data() + y * kHostFrameWidth * 4u;
            for (uint32_t x = 0; x < width; ++x)
            {
                if (row[x * 4u] != 0u || row[x * 4u + 1u] != 0u || row[x * 4u + 2u] != 0u)
                    ++count;
            }
        }
        return count;
    }
}

GSCpuBackend::GSCpuBackend()
{
    using namespace GSMem;
    static std::once_flag lookupTablesOnce;
    std::call_once(lookupTablesOnce, []()
                   { InitLookupTables(); });
    for (size_t i = 0; i < kPsmHandlerCount; ++i)
    {
        switch (i)
        {
        case GS_PSM_CT32:
            m_readVramFuncs[i] = ReadCT32;
            m_writeVramFuncs[i] = WriteCT32;
            break;
        case GS_PSM_CT24:
            m_readVramFuncs[i] = ReadCT24;
            m_writeVramFuncs[i] = WriteCT24;
            break;
        case GS_PSM_CT16:
            m_readVramFuncs[i] = ReadCT16;
            m_writeVramFuncs[i] = WriteCT16;
            break;
        case GS_PSM_CT16S:
            m_readVramFuncs[i] = ReadCT16S;
            m_writeVramFuncs[i] = WriteCT16S;
            break;
        case GS_PSM_T8:
            m_readVramFuncs[i] = ReadP8;
            m_writeVramFuncs[i] = WriteP8;
            break;
        case GS_PSM_T8H:
            m_readVramFuncs[i] = ReadP8H;
            m_writeVramFuncs[i] = WriteP8H;
            break;
        case GS_PSM_T4:
            m_readVramFuncs[i] = ReadP4;
            m_writeVramFuncs[i] = WriteP4;
            break;
        case GS_PSM_T4HH:
            m_readVramFuncs[i] = ReadP4HH;
            m_writeVramFuncs[i] = WriteP4HH;
            break;
        case GS_PSM_T4HL:
            m_readVramFuncs[i] = ReadP4HL;
            m_writeVramFuncs[i] = WriteP4HL;
            break;
        case GS_PSM_Z32:
            m_readVramFuncs[i] = ReadZ32;
            m_writeVramFuncs[i] = WriteZ32;
            break;
        case GS_PSM_Z24:
            m_readVramFuncs[i] = ReadZ24;
            m_writeVramFuncs[i] = WriteZ24;
            break;
        case GS_PSM_Z16:
            m_readVramFuncs[i] = ReadZ16;
            m_writeVramFuncs[i] = WriteZ16;
            break;
        case GS_PSM_Z16S:
            m_readVramFuncs[i] = ReadZ16S;
            m_writeVramFuncs[i] = WriteZ16S;
            break;
        default:
            m_readVramFuncs[i] = ReadNull;
            m_writeVramFuncs[i] = WriteNull;
            break;
        }
    }
    Reset();
}

void GSCpuBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    if (vram && vramSize < GSMem::MEMORY_SIZE)
        throw std::invalid_argument("GS CPU backend requires at least 4 MiB of VRAM");

    std::lock_guard<std::mutex> lock(m_mutex);
    m_vram = vram;
    m_vramSize = vramSize;
    ResetUnlocked();
}

void GSCpuBackend::Reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ResetUnlocked();
}

void GSCpuBackend::ResetUnlocked()
{
    m_clut.fill(0u);
    m_clutCbp.fill(0u);
    m_texturePageCache.Invalidate();
    m_transfer = {};
    m_transfer.direction = 3u;
    m_transferState = {};
    m_transferState.direction = 3u;
    m_localToHostBuffer.clear();
    m_localToHostReadPos = 0u;
}

void GSCpuBackend::Submit(const GSPrimitiveBatch &batch)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || batch.vertexCount == 0u)
        return;
    DrawPrimitive(batch);
}

void GSCpuBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || (!isFourBitIndexedPsm(tex0.psm) && !isEightBitIndexedPsm(tex0.psm)))
        return;

    // DIAG 2026-10-18 (Gemini v76): con SWORD_CLUT_RELOAD=1 se recarga la paleta en CADA escritura de
    // TEX0, ignorando el cache m_clutCbp (que no se invalida en las subidas a VRAM).
    if (std::getenv("SWORD_CLUT_RELOAD") != nullptr)
    {
        LoadClutUnlocked(tex0, texclut);
        return;
    }
    switch (tex0.cld)
    {
    case 0u:
    case 6u:
    case 7u:
        return;
    case 1u:
        break;
    case 2u:
        m_clutCbp[0] = tex0.cbp;
        break;
    case 3u:
        m_clutCbp[1] = tex0.cbp;
        break;
    case 4u:
        if (m_clutCbp[0] == tex0.cbp)
            return;
        m_clutCbp[0] = tex0.cbp;
        break;
    case 5u:
        if (m_clutCbp[1] == tex0.cbp)
            return;
        m_clutCbp[1] = tex0.cbp;
        break;
    default:
        return;
    }

    LoadClutUnlocked(tex0, texclut);
}

void GSCpuBackend::LoadClutUnlocked(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    const bool fourBit = isFourBitIndexedPsm(tex0.psm);
    const bool sixteenBit = tex0.cpsm == GS_PSM_CT16 || tex0.cpsm == GS_PSM_CT16S;
    const bool thirtyTwoBit = tex0.cpsm == GS_PSM_CT32 || tex0.cpsm == GS_PSM_CT24;
    if (!sixteenBit && !thirtyTwoBit)
        return;

    const uint32_t entryCount = fourBit ? 16u : 256u;
    const uint32_t csaMask = sixteenBit ? 0x1Fu : 0x0Fu;
    const uint32_t destinationBase = (static_cast<uint32_t>(tex0.csa) & csaMask) << 4u;

    const bool loadCsm1Suffix = tex0.csm == 0u && thirtyTwoBit && !fourBit;
    const uint32_t firstEntry = loadCsm1Suffix ? destinationBase : 0u;

    for (uint32_t entry = firstEntry; entry < entryCount; ++entry)
    {
        uint32_t sourceX = 0u;
        uint32_t sourceY = 0u;
        uint32_t sourceWidth = 1u;

        if (tex0.csm == 0u)
        {
            const uint32_t sourceIndex = swizzleClutIndexCSM1(entry);
            sourceX = sourceIndex & 0x0Fu;
            sourceY = sourceIndex >> 4u;
        }
        else
        {
            sourceWidth = texclut.cbw != 0u ? static_cast<uint32_t>(texclut.cbw) : 1u;
            sourceX = (static_cast<uint32_t>(texclut.cou) << 4u) + entry;
            sourceY = static_cast<uint32_t>(texclut.cov);
        }

        const uint32_t raw = ReadTextureVramUnlocked(tex0.cpsm, tex0.cbp, sourceWidth, sourceX, sourceY); 
        const uint32_t destination = (loadCsm1Suffix ? entry : destinationBase + entry) & (sixteenBit ? 0x1FFu : 0x0FFu);
        if (sixteenBit)
        {
            m_clut[destination] = static_cast<uint16_t>(raw);
        }
        else
        {
            m_clut[destination] = static_cast<uint16_t>(raw & 0xFFFFu);
            m_clut[destination + 256u] = static_cast<uint16_t>(raw >> 16u);
        }
    }

    // DIAG 2026-10-18 (Gemini v77): verificar que la recarga lee la paleta correcta de VRAM.
    {
        static uint32_t s_clLoadLog = 0u;
        if (s_clLoadLog++ < 8u)
            std::cout << "[clut-load] cbp=" << tex0.cbp << " vram=0x" << std::hex << (tex0.cbp * 256u)
                      << " e1=0x" << ReadTextureVramUnlocked(tex0.cpsm, tex0.cbp, 1u, 1u, 0u)
                      << std::dec << std::endl;
    }
}

void GSCpuBackend::Flush()
{
    // CPU backend is immediate. GPU backends may submit command buffers here.
}

void GSCpuBackend::TextureFlush()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_texturePageCache.Invalidate();
}

void GSCpuBackend::Sync(GSSyncReason)
{
    // CPU backend is immediate. GPU backends may wait on fences/readbacks here.
}

uint32_t GSCpuBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return ReadVramUnlocked(psm, base, bw, x, y);
}

uint32_t GSCpuBackend::ReadVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    if (!m_vram)
        return 0u;
    return m_readVramFuncs[psm & 0x3Fu](m_vram, base, bw, x, y);
}

uint32_t GSCpuBackend::ReadTextureVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y)
{
    if (!m_vram)
        return 0u;

    return GSMem::ReadTexture(m_texturePageCache, m_vram, psm, base, bw, x, y);
}

void GSCpuBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    WriteVramUnlocked(psm, base, bw, x, y, value);
}

void GSCpuBackend::WriteVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    if (!m_vram)
        return;
    m_writeVramFuncs[psm & 0x3Fu](m_vram, base, bw, x, y, value);
}

void GSCpuBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || m_vramSize == 0u)
    {
        out.clear();
        return;
    }
    out.resize(m_vramSize);
    std::memcpy(out.data(), m_vram, m_vramSize);
}

GSTransferSnapshot GSCpuBackend::GetTransferSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    GSTransferSnapshot result = m_transferState;
    result.localToHostPendingBytes = m_localToHostReadPos < m_localToHostBuffer.size()
                                         ? m_localToHostBuffer.size() - m_localToHostReadPos
                                         : 0u;
    return result;
}

void GSCpuBackend::DrawPrimitive(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const auto &ctx = state.context;
    // 2026-10-19 (hito 1.2): prims emitidos por el callback de render en ejecucion (tag en
    // game_overrides.cpp). Env: SWORD_UI_CB_PRIMS=1 (OFF por defecto).
    {
        struct UiPrimCfg
        {
            bool on = false;
        };
        static const UiPrimCfg s_uiPrims = []() {
            UiPrimCfg c;
            const char *e = std::getenv("SWORD_UI_CB_PRIMS");
            c.on = (e != nullptr && *e != '\0' && std::strcmp(e, "0") != 0);
            return c;
        }();
        if (s_uiPrims.on && (g_swordUiCbTag != 0u || g_swordUiCurWidget != 0u))
        {
            static std::unordered_map<uint32_t, uint32_t> s_uiPrimN;
            const uint32_t key = (g_swordUiCbTag != 0u) ? (0x80000000u | g_swordUiCbTag)
                                                        : g_swordUiCurWidget;
            uint32_t &n = s_uiPrimN[key];
            ++n;
            if (n <= 12u || (n % 1000u) == 0u)
            {
                std::cout << (g_swordUiCbTag != 0u ? "[ui-cb-prim]" : "[ui-widget-prim]")
                          << " cb=0x" << std::hex << g_swordUiCbTag
                          << " widget=0x" << g_swordUiCurWidget
                          << std::dec << " n=" << n
                          << " type=" << static_cast<uint32_t>(state.prim.type)
                          << " tme=" << static_cast<uint32_t>(state.prim.tme)
                          << " fst=" << static_cast<uint32_t>(state.prim.fst)
                          << " fbp=" << ctx.frame.fbp
                          << " tbp0=" << ctx.tex0.tbp0
                          << " psm=" << static_cast<uint32_t>(ctx.tex0.psm)
                          << " cbp=" << ctx.tex0.cbp
                          << " cpsm=" << static_cast<uint32_t>(ctx.tex0.cpsm)
                          << " uv0=(" << (batch.vertices[0].u >> 4) << ","
                          << (batch.vertices[0].v >> 4) << ")"
                          << " stq0=(" << batch.vertices[0].s << ","
                          << batch.vertices[0].t << "," << batch.vertices[0].q << ")"
                          << " rgba0=(" << static_cast<uint32_t>(batch.vertices[0].r) << ","
                          << static_cast<uint32_t>(batch.vertices[0].g) << ","
                          << static_cast<uint32_t>(batch.vertices[0].b) << ","
                          << static_cast<uint32_t>(batch.vertices[0].a) << ")"
                          << std::endl;
            }
        }
    }
    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t primitiveIndex = s_debugPrimitiveCount.fetch_add(1u, std::memory_order_relaxed);
        if (primitiveIndex < 64u)
        {
            std::cout << "[gs:prim] idx=" << primitiveIndex
                      << " type=" << static_cast<uint32_t>(state.prim.type)
                      << " tme=" << static_cast<uint32_t>(state.prim.tme)
                      << " abe=" << static_cast<uint32_t>(state.prim.abe)
                      << " fst=" << static_cast<uint32_t>(state.prim.fst)
                      << " ctxt=" << static_cast<uint32_t>(state.prim.ctxt)
                      << " fbp=" << ctx.frame.fbp
                      << " fbw=" << ctx.frame.fbw
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.frame.psm) << std::dec
                      << " tex0=("
                      << "tbp0=" << ctx.tex0.tbp0
                      << " tbw=" << static_cast<uint32_t>(ctx.tex0.tbw)
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.psm) << std::dec
                      << " tw=" << static_cast<uint32_t>(ctx.tex0.tw)
                      << " th=" << static_cast<uint32_t>(ctx.tex0.th)
                      << " tcc=" << static_cast<uint32_t>(ctx.tex0.tcc)
                      << " tfx=" << static_cast<uint32_t>(ctx.tex0.tfx)
                      << " cbp=" << ctx.tex0.cbp
                      << " cpsm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.cpsm) << std::dec
                      << " csm=" << static_cast<uint32_t>(ctx.tex0.csm)
                      << " csa=" << static_cast<uint32_t>(ctx.tex0.csa)
                      << ")"
                      << " texclut=("
                      << "cbw=" << static_cast<uint32_t>(state.texclut.cbw)
                      << " cou=" << static_cast<uint32_t>(state.texclut.cou)
                      << " cov=" << state.texclut.cov
                      << ")"
                      << " ofx=" << (ctx.xyoffset.ofx >> 4)
                      << " ofy=" << (ctx.xyoffset.ofy >> 4)
                      << " scissor=(" << ctx.scissor.x0
                      << "," << ctx.scissor.y0
                      << ")-(" << ctx.scissor.x1
                      << "," << ctx.scissor.y1 << ")"
                      << " test=0x" << std::hex << ctx.test
                      << " alpha=0x" << ctx.alpha
                      << std::dec
                      << " v0=(" << batch.vertices[0].x << "," << batch.vertices[0].y << ")"
                      << " uv0=(" << (batch.vertices[0].u >> 4) << "," << (batch.vertices[0].v >> 4) << ")"
                      << " stq0=(" << batch.vertices[0].s << "," << batch.vertices[0].t << "," << batch.vertices[0].q << ")"
                      << " v1=(" << batch.vertices[1].x << "," << batch.vertices[1].y << ")"
                      << " uv1=(" << (batch.vertices[1].u >> 4) << "," << (batch.vertices[1].v >> 4) << ")"
                      << " stq1=(" << batch.vertices[1].s << "," << batch.vertices[1].t << "," << batch.vertices[1].q << ")"
                      << " v2=(" << batch.vertices[2].x << "," << batch.vertices[2].y << ")"
                      << " uv2=(" << (batch.vertices[2].u >> 4) << "," << (batch.vertices[2].v >> 4) << ")"
                      << " stq2=(" << batch.vertices[2].s << "," << batch.vertices[2].t << "," << batch.vertices[2].q << ")"
                      << " rgba0=(" << static_cast<uint32_t>(batch.vertices[0].r) << ","
                      << static_cast<uint32_t>(batch.vertices[0].g) << ","
                      << static_cast<uint32_t>(batch.vertices[0].b) << ","
                      << static_cast<uint32_t>(batch.vertices[0].a) << ")"
                      << " rgba1=(" << static_cast<uint32_t>(batch.vertices[1].r) << ","
                      << static_cast<uint32_t>(batch.vertices[1].g) << ","
                      << static_cast<uint32_t>(batch.vertices[1].b) << ","
                      << static_cast<uint32_t>(batch.vertices[1].a) << ")"
                      << " rgba2=(" << static_cast<uint32_t>(batch.vertices[2].r) << ","
                      << static_cast<uint32_t>(batch.vertices[2].g) << ","
                      << static_cast<uint32_t>(batch.vertices[2].b) << ","
                      << static_cast<uint32_t>(batch.vertices[2].a) << ")"
                      << std::endl;
        }
    });

    PS2_IF_AGRESSIVE_LOGS({
        if ((state.prim.ctxt != 0u || ctx.frame.fbp == 150u) &&
            s_debugContext1PrimitiveCount.fetch_add(1u, std::memory_order_relaxed) < 32u)
        {
            std::cout << "[gs:copy-prim]"
                      << " type=" << static_cast<uint32_t>(state.prim.type)
                      << " tme=" << static_cast<uint32_t>(state.prim.tme)
                      << " abe=" << static_cast<uint32_t>(state.prim.abe)
                      << " fst=" << static_cast<uint32_t>(state.prim.fst)
                      << " ctxt=" << static_cast<uint32_t>(state.prim.ctxt)
                      << " fbp=" << ctx.frame.fbp
                      << " fbw=" << ctx.frame.fbw
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.frame.psm) << std::dec
                      << " tex0=("
                      << "tbp0=" << ctx.tex0.tbp0
                      << " tbw=" << static_cast<uint32_t>(ctx.tex0.tbw)
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.psm) << std::dec
                      << " tcc=" << static_cast<uint32_t>(ctx.tex0.tcc)
                      << " tfx=" << static_cast<uint32_t>(ctx.tex0.tfx)
                      << " cbp=" << ctx.tex0.cbp
                      << " cpsm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.cpsm) << std::dec
                      << " csm=" << static_cast<uint32_t>(ctx.tex0.csm)
                      << " csa=" << static_cast<uint32_t>(ctx.tex0.csa)
                      << ")"
                      << " texclut=("
                      << "cbw=" << static_cast<uint32_t>(state.texclut.cbw)
                      << " cou=" << static_cast<uint32_t>(state.texclut.cou)
                      << " cov=" << state.texclut.cov
                      << ")"
                      << " ofx=" << (ctx.xyoffset.ofx >> 4)
                      << " ofy=" << (ctx.xyoffset.ofy >> 4)
                      << " scissor=(" << ctx.scissor.x0
                      << "," << ctx.scissor.y0
                      << ")-(" << ctx.scissor.x1
                      << "," << ctx.scissor.y1 << ")"
                      << " test=0x" << std::hex << ctx.test
                      << " alpha=0x" << ctx.alpha
                      << std::dec << std::endl;
        }
    });

    switch (batch.state.prim.type)
    {
    case GS_PRIM_SPRITE:
        DrawSprite(batch);
        break;
    case GS_PRIM_TRIANGLE:
    case GS_PRIM_TRISTRIP:
    case GS_PRIM_TRIFAN:
        DrawTriangle(batch);
        break;
    case GS_PRIM_LINE:
    case GS_PRIM_LINESTRIP:
        DrawLine(batch);
        break;
    case GS_PRIM_POINT:
    {
        const GSVertex &v = batch.vertices[0];
        const auto &ctx = batch.state.context;
        int px = static_cast<int>(v.x) - (ctx.xyoffset.ofx >> 4);
        int py = static_cast<int>(v.y) - (ctx.xyoffset.ofy >> 4);
        WritePixel(batch.state, px, py, static_cast<u32>(v.z), v.r, v.g, v.b, v.a, v.fog);
        break;
    }
    default:
        break;
    }
}

void GSCpuBackend::WritePixel(const GSDrawState &state, int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint8_t fog)
{
    const auto &ctx = state.context;
    if (x < ctx.scissor.x0 || x > ctx.scissor.x1 || y < ctx.scissor.y0 || y > ctx.scissor.y1)
        return;

    if (state.prim.fge)
    {
        const uint32_t inverseFog = 255u - fog;
        auto applyFog = [&](uint8_t input, uint8_t fogColor) -> uint8_t
        {
            return static_cast<uint8_t>(((static_cast<uint32_t>(fog) * input) >> 8) + ((inverseFog * fogColor) >> 8));
        };

        r = applyFog(r, state.fogR);
        g = applyFog(g, state.fogG);
        b = applyFog(b, state.fogB);
    }

    const u32 fbp = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
    const u32 fbw = std::max<u32>(ctx.frame.fbw, 1u);
    const u32 fpsm = ctx.frame.psm;
    const u32 zbp = GSInternal::framePageBaseToBlock(ctx.zbuf.zbp);
    const u32 zpsm = ctx.zbuf.psm;

    PixelWriteMask writeMask = classifyAlphaTest(ctx.test, a, static_cast<uint8_t>(fpsm));
    // 2026-10-20 (hito 1.6): ¿el sprite de clear full-screen (RGBA 0, tme=0, abe=0) se descarta?
    const bool clearCandidate = state.prim.type == GS_PRIM_SPRITE && !state.prim.tme &&
                                !state.prim.abe && r == 0u && g == 0u && b == 0u && a == 0u;
    {
        static const bool s_clearProbe = []() {
            const char *e = std::getenv("SWORD_CLEAR_PROBE");
            return e != nullptr && *e != '\0' && std::strcmp(e, "0") != 0;
        }();
        static const bool s_clearForce = []() {
            const char *e = std::getenv("SWORD_CLEAR_FORCE");
            return e != nullptr && *e != '\0' && std::strcmp(e, "0") != 0;
        }();
        if (clearCandidate && s_clearProbe)
        {
            static uint32_t s_n = 0u, s_discard = 0u;
            if (s_n++ < 20u)
                std::cout << "[clear-probe] xy=(" << x << "," << y << ")"
                          << " test=0x" << std::hex << ctx.test << std::dec
                          << " mask=" << writeMask.writeRgb << writeMask.writeAlpha << writeMask.writeDepth
                          << " fbp=" << ctx.frame.fbp << " fbw=" << ctx.frame.fbw
                          << " fbmsk=0x" << std::hex << ctx.frame.fbmsk << std::dec
                          << " scissor=(" << ctx.scissor.x0 << "," << ctx.scissor.y0 << ")-("
                          << ctx.scissor.x1 << "," << ctx.scissor.y1 << ")" << std::endl;
            if (!writeMask.writesFramebuffer() && s_discard++ < 20u)
                std::cout << "[clear-probe] DISCARD xy=(" << x << "," << y << ")"
                          << " test=0x" << std::hex << ctx.test << std::dec << std::endl;
        }
        if (clearCandidate && s_clearForce && !writeMask.writesFramebuffer())
        {
            writeMask.writeRgb = true;
            writeMask.writeAlpha = true;
            writeMask.writeDepth = false;
        }
    }
    if (!writeMask.writesAnything())
    {
        // DIAG 2026-10-18: ¿los sprites texturizados se DESCARTAN por alpha test (alpha=0)?
        static uint32_t s_discLog = 0u;
        if (state.prim.tme && s_discLog++ < 10u)
            std::cout << "[raster-probe] DISCARD tme=1 dest fbp=" << ctx.frame.fbp << " fbw=" << ctx.frame.fbw
                      << " psm=" << fpsm << " xy=(" << x << "," << y << ")"
                      << " rgba=(" << static_cast<uint32_t>(r) << "," << static_cast<uint32_t>(g) << ","
                      << static_cast<uint32_t>(b) << "," << static_cast<uint32_t>(a) << ")"
                      << " test=0x" << std::hex << ctx.test << " alpha=0x" << ctx.alpha << std::dec << std::endl;
        return;
    }

    const uint32_t ztestMethod = static_cast<uint32_t>((ctx.test >> 17) & 3u);
    const bool alphaBlendEnabled = state.prim.abe;
    const bool preserveDestinationAlpha = writeMask.writeRgb && !writeMask.writeAlpha && fpsm == GS_PSM_CT32;
    const bool destinationAlphaTestNeedsRead = ((ctx.test >> 14) & 0x1u) != 0u && (fpsm == GS_PSM_CT32 || fpsm == GS_PSM_CT16 || fpsm == GS_PSM_CT16S);

    // small optimization, avoid reading the framebuffer for simple draws
    // TODO: only one address lookup for rmw
    const bool frmw = destinationAlphaTestNeedsRead || (writeMask.writesFramebuffer() && ((ctx.frame.fbmsk != 0) || alphaBlendEnabled || preserveDestinationAlpha));

    u32 rawFramebufferPixel = 0;
    u32 fbrgba = 0;
    if (frmw)
    {
        rawFramebufferPixel = ReadVramUnlocked(fpsm, fbp, fbw, x, y);
        fbrgba = rawFramebufferPixel;

        if (bitsPerPixel(fpsm) == 16)
        {
            fbrgba = Rgba5551ToRgba8888(fbrgba);
        }
        else if (fpsm == GS_PSM_CT24)
        {
            // The GS supplies 0x80 as destination alpha for RGB24 blending.
            fbrgba |= 0x80000000u;
        }
    }

    if (!passesDestinationAlphaTest(ctx.test, static_cast<uint8_t>(fpsm), rawFramebufferPixel))
    {
        return;
    }

    bool zpass = false;
    uint32_t storedZ = 0u;
    switch (ztestMethod)
    {
    case 0:
        zpass = false;
        break;
    case 1:
        zpass = true;
        break;
    case 2:
        storedZ = ReadVramUnlocked(zpsm, zbp, fbw, x, y);
        zpass = static_cast<uint32_t>(z) >= storedZ;
        break;
    case 3:
        storedZ = ReadVramUnlocked(zpsm, zbp, fbw, x, y);
        zpass = static_cast<uint32_t>(z) > storedZ;
        break;
    }

    if (!zpass)
    {
        return;
    }

    if (writeMask.writesFramebuffer())
    {
        const u8 srcR = r;
        const u8 srcG = g;
        const u8 srcB = b;

        if (state.prim.abe)
        {
            uint8_t dr = fbrgba & 0xFF;
            uint8_t dg = (fbrgba >> 8) & 0xFF;
            uint8_t db = (fbrgba >> 16) & 0xFF;
            uint8_t da = (fbrgba >> 24) & 0xFF;

            // PABE disables alpha blending when the source alpha MSB is clear.
            if (!(state.pabe && (a & 0x80u) == 0u))
            {
                uint64_t alphaReg = ctx.alpha;
                uint8_t asel = alphaReg & 3;
                uint8_t bsel = (alphaReg >> 2) & 3;
                uint8_t csel = (alphaReg >> 4) & 3;
                uint8_t dsel = (alphaReg >> 6) & 3;
                uint8_t fix = static_cast<uint8_t>((alphaReg >> 32) & 0xFF);

                auto pickRGB = [&](uint8_t sel, int cs, int cd) -> int
                {
                    if (sel == 0)
                        return cs;
                    if (sel == 1)
                        return cd;
                    return 0;
                };
                int cAlpha = (csel == 0) ? a : (csel == 1) ? da
                                                           : fix;

                r = clampU8(((pickRGB(asel, r, dr) - pickRGB(bsel, r, dr)) * cAlpha >> 7) + pickRGB(dsel, r, dr));
                g = clampU8(((pickRGB(asel, g, dg) - pickRGB(bsel, g, dg)) * cAlpha >> 7) + pickRGB(dsel, g, dg));
                b = clampU8(((pickRGB(asel, b, db) - pickRGB(bsel, b, db)) * cAlpha >> 7) + pickRGB(dsel, b, db));
            }
            else
            {
                r = srcR;
                g = srcG;
                b = srcB;
            }
        }

        if (writeMask.writeAlpha && (ctx.fba & 0x1ull) != 0ull && ctx.frame.psm != GS_PSM_CT24)
        {
            a = static_cast<uint8_t>(a | 0x80u);
        }

        u32 pixel = pack32(r, g, b, a);

        if (ctx.frame.fbmsk != 0)
        {
            pixel = (pixel & ~ctx.frame.fbmsk) | (fbrgba & ctx.frame.fbmsk);
        }

        if (preserveDestinationAlpha)
        {
            pixel = (pixel & 0x00FFFFFFu) | (fbrgba & 0xFF000000u);
        }

        // format conversion
        if (bitsPerPixel(fpsm) == 16)
        {
            pixel = Rgba8888ToRgba5551(pixel);
        }

        WriteVramUnlocked(fpsm, fbp, fbw, x, y, pixel);
    }

    if (writeMask.writeDepth && !ctx.zbuf.zmask)
    {
        WriteVramUnlocked(zpsm, zbp, fbw, x, y, z);
    }
}

uint32_t GSCpuBackend::LookupCLUT(const GSDrawState &state,
                                  uint8_t index,
                                  uint8_t cpsm,
                                  uint8_t csm,
                                  uint8_t csa,
                                  uint8_t sourcePsm)
{
    const bool sixteenBit = cpsm == GS_PSM_CT16 || cpsm == GS_PSM_CT16S;
    const uint32_t csaMask = sixteenBit ? 0x1Fu : 0x0Fu;
    const uint32_t clutBase = (static_cast<uint32_t>(csa) & csaMask) << 4u;
    const uint32_t sourceIndex = isFourBitIndexedPsm(sourcePsm)
                                     ? (static_cast<uint32_t>(index) & 0x0Fu)
                                     : static_cast<uint32_t>(index);

    uint32_t clutIndex = (clutBase + sourceIndex) & (sixteenBit ? 0x1FFu : 0x0FFu);
    if (!sixteenBit && csm == 0u && isEightBitIndexedPsm(sourcePsm))
    {
        const uint32_t block = std::min((sourceIndex & 0xF0u) + clutBase, 240u);
        clutIndex = block + (sourceIndex & 0x0Fu);
    }

    switch (cpsm)
    {
    case GS_PSM_CT32:
    {
        // DIAG 2026-10-18: ¿la entrada de CLUT es 0 (cache vacio) o el indice no la alcanza?
        {
            static uint32_t s_clN = 0u;
            if (s_clN++ < 8u)
                std::cout << "[clut-probe] idx=" << static_cast<uint32_t>(index)
                          << " clutIndex=" << clutIndex
                          << " c0=" << m_clut[clutIndex] << " c256=" << m_clut[clutIndex + 256u]
                          << " csm=" << static_cast<uint32_t>(csm)
                          << " csa=" << static_cast<uint32_t>(csa)
                          << " cbp0=" << m_clutCbp[0] << " cbp1=" << m_clutCbp[1] << std::endl;
        }
        const uint32_t raw = static_cast<uint32_t>(m_clut[clutIndex]) | (static_cast<uint32_t>(m_clut[clutIndex + 256u]) << 16u);
        return applyTexa(state.texa, cpsm, raw);
    }
    case GS_PSM_CT24:
    {
        const uint32_t raw = static_cast<uint32_t>(m_clut[clutIndex]) | (static_cast<uint32_t>(m_clut[clutIndex + 256u]) << 16u);
        return applyTexa(state.texa, cpsm, raw & 0x00FFFFFFu);
    }
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
        return applyTexa(state.texa, cpsm, Rgba5551ToRgba8888(m_clut[clutIndex]));
    default:
        break;
    }

    return 0xFFFF00FFu;
}

uint32_t GSCpuBackend::SampleTexture(const GSDrawState &state, float s, float t, float q, uint16_t u, uint16_t v)
{
    const auto &ctx = state.context;
    const auto &tex = ctx.tex0;

    const int texW = state.textureWidth;
    const int texH = state.textureHeight;
    const uint64_t clamp = ctx.clamp;
    const uint8_t wrapU = static_cast<uint8_t>(clamp & 0x3u);
    const uint8_t wrapV = static_cast<uint8_t>((clamp >> 2) & 0x3u);
    const uint16_t minU = static_cast<uint16_t>((clamp >> 4) & 0x3FFu);
    const uint16_t maxU = static_cast<uint16_t>((clamp >> 14) & 0x3FFu);
    const uint16_t minV = static_cast<uint16_t>((clamp >> 24) & 0x3FFu);
    const uint16_t maxV = static_cast<uint16_t>((clamp >> 34) & 0x3FFu);

    float texUf, texVf;
    if (state.prim.fst)
    {
        texUf = static_cast<float>(u) / 16.0f;
        texVf = static_cast<float>(v) / 16.0f;
    }
    else
    {
        const float invQ = 1.0f / fabsQ(q);
        texUf = s * invQ * static_cast<float>(texW);
        texVf = t * invQ * static_cast<float>(texH);
    }

    auto samplePoint = [&](int sampleU, int sampleV) -> uint32_t
    {
        sampleU = wrapTextureCoordinate(sampleU, texW, wrapU, minU, maxU);
        sampleV = wrapTextureCoordinate(sampleV, texH, wrapV, minV, maxV);

        u32 out = ReadTextureVramUnlocked(tex.psm, tex.tbp0, tex.tbw, sampleU, sampleV);

        // DIAG 2026-10-18: valor crudo del texel (indice en PSMT8) para ver si el 0 viene del indice
        // o del lookup de la CLUT (que se sirve solo desde el cache m_clut).
        {
            static uint32_t s_txN = 0u;
            if (s_txN++ < 8u)
                std::cout << "[tex-probe] raw=0x" << std::hex << out << std::dec
                          << " su=" << sampleU << " sv=" << sampleV
                          << " psm=" << static_cast<uint32_t>(tex.psm) << " tbp0=" << tex.tbp0
                          << " tbw=" << static_cast<uint32_t>(tex.tbw)
                          << " texW=" << texW << " texH=" << texH
                          << " cbp=" << tex.cbp << " cpsm=" << static_cast<uint32_t>(tex.cpsm) << std::endl;
        }

        // DIAG 2026-10-18 (Gemini v77): indice crudo + valor de CLUT para el atlas (tbp0=10272).
        if (tex.tbp0 == 10272u)
        {
            static uint32_t s_sdN = 0u;
            if (s_sdN++ < 12u)
                std::cout << "[sample-diag] su=" << sampleU << " sv=" << sampleV
                          << " rawIdx=" << static_cast<uint32_t>(static_cast<uint8_t>(out))
                          << " clutVal=0x" << std::hex
                          << LookupCLUT(state, static_cast<uint8_t>(out), tex.cpsm, tex.csm, tex.csa, tex.psm)
                          << std::dec << std::endl;
        }

        switch (tex.psm)
        {
        case GS_PSM_CT32:
        case GS_PSM_Z32:
        case GS_PSM_CT24:
        case GS_PSM_Z24:
            return applyTexa(state.texa, tex.psm, out);
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            return applyTexa(state.texa, tex.psm, Rgba5551ToRgba8888(out));
        case GS_PSM_T8:
        case GS_PSM_T8H:
        case GS_PSM_T4:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
            return LookupCLUT(state, static_cast<u8>(out), tex.cpsm, tex.csm, tex.csa, tex.psm);
        }

        return 0xFFFF00FFu;
    };

    if (!state.linearFilter)
    {
        // NEAREST: la spec del GS TRUNCA (no redondea). PCSX2 GSDrawScanline.cpp:835-844 aplica el
        // sesgo de medio texel (-0x8000) SOLO bajo `if (sel.ltf)`, y deriva el texel con
        // `uv0 = u.sra32<16>()` (shift aritmético = truncamiento hacia -inf).
        // NO sumar 0.5 aquí: seria un desplazamiento de medio texel (dibujaria lo que el real descarta).
        return samplePoint(static_cast<int>(texUf), static_cast<int>(texVf));
    }

    const float sampleU = texUf - 0.5f;
    const float sampleV = texVf - 0.5f;
    const int u0 = static_cast<int>(std::floor(sampleU));
    const int v0 = static_cast<int>(std::floor(sampleV));
    const int u1 = u0 + 1;
    const int v1 = v0 + 1;
    const float fx = sampleU - static_cast<float>(u0);
    const float fy = sampleV - static_cast<float>(v0);

    const uint32_t c00 = samplePoint(u0, v0);
    const uint32_t c10 = samplePoint(u1, v0);
    const uint32_t c01 = samplePoint(u0, v1);
    const uint32_t c11 = samplePoint(u1, v1);

    const uint8_t r = lerpChannel(static_cast<uint8_t>(c00 & 0xFFu),
                                  static_cast<uint8_t>(c10 & 0xFFu),
                                  static_cast<uint8_t>(c01 & 0xFFu),
                                  static_cast<uint8_t>(c11 & 0xFFu),
                                  fx, fy);
    const uint8_t g = lerpChannel(static_cast<uint8_t>((c00 >> 8) & 0xFFu),
                                  static_cast<uint8_t>((c10 >> 8) & 0xFFu),
                                  static_cast<uint8_t>((c01 >> 8) & 0xFFu),
                                  static_cast<uint8_t>((c11 >> 8) & 0xFFu),
                                  fx, fy);
    const uint8_t b = lerpChannel(static_cast<uint8_t>((c00 >> 16) & 0xFFu),
                                  static_cast<uint8_t>((c10 >> 16) & 0xFFu),
                                  static_cast<uint8_t>((c01 >> 16) & 0xFFu),
                                  static_cast<uint8_t>((c11 >> 16) & 0xFFu),
                                  fx, fy);
    const uint8_t a = lerpChannel(static_cast<uint8_t>((c00 >> 24) & 0xFFu),
                                  static_cast<uint8_t>((c10 >> 24) & 0xFFu),
                                  static_cast<uint8_t>((c01 >> 24) & 0xFFu),
                                  static_cast<uint8_t>((c11 >> 24) & 0xFFu),
                                  fx, fy);

    return static_cast<uint32_t>(r) |
           (static_cast<uint32_t>(g) << 8) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(a) << 24);
}

void GSCpuBackend::DrawSprite(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    const auto &ctx = state.context;

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    int x0 = static_cast<int>(v0.x) - ofx;
    int y0 = static_cast<int>(v0.y) - ofy;
    int x1 = static_cast<int>(v1.x) - ofx;
    int y1 = static_cast<int>(v1.y) - ofy;
    u32 z1 = static_cast<u32>(v1.z);

    if (x0 > x1)
        std::swap(x0, x1);
    if (y0 > y1)
        std::swap(y0, y1);

    const int unclippedX0 = x0;
    const int unclippedY0 = y0;
    const int spanX = std::max(1, x1 - x0);
    const int spanY = std::max(1, y1 - y0);
    const int unclippedX1 = unclippedX0 + spanX - 1;
    const int unclippedY1 = unclippedY0 + spanY - 1;

    // If the sprite rectangle is fully outside scissor, nothing should render.
    if (unclippedX1 < ctx.scissor.x0 || unclippedX0 > ctx.scissor.x1 ||
        unclippedY1 < ctx.scissor.y0 || unclippedY0 > ctx.scissor.y1)
        return;

    const int drawX0 = clampInt(unclippedX0, ctx.scissor.x0, ctx.scissor.x1);
    const int drawY0 = clampInt(unclippedY0, ctx.scissor.y0, ctx.scissor.y1);
    const int drawX1 = clampInt(unclippedX1, ctx.scissor.x0, ctx.scissor.x1);
    const int drawY1 = clampInt(unclippedY1, ctx.scissor.y0, ctx.scissor.y1);

    // DIAG 2026-10-18: ¿donde dibujan los sprites texturizados y con que color/alpha de vertice?
    {
        static uint32_t s_dsN = 0u;
        if (state.prim.tme && s_dsN++ < 10u)
            std::cout << "[raster-probe] sprite tme=1 abe=" << static_cast<uint32_t>(state.prim.abe)
                      << " fst=" << static_cast<uint32_t>(state.prim.fst)
                      << " dest fbp=" << ctx.frame.fbp << " fbw=" << ctx.frame.fbw
                      << " psm=" << static_cast<uint32_t>(ctx.frame.psm)
                      << " rect=(" << unclippedX0 << "," << unclippedY0 << ")-(" << unclippedX1 << "," << unclippedY1 << ")"
                      << " ctr=" << (drawX1 - drawX0 + 1) << "x" << (drawY1 - drawY0 + 1)
                      << " scissor=(" << ctx.scissor.x0 << "," << ctx.scissor.y0 << ")-(" << ctx.scissor.x1 << "," << ctx.scissor.y1 << ")"
                      << " rgba0=(" << static_cast<uint32_t>(v0.r) << "," << static_cast<uint32_t>(v0.g) << ","
                      << static_cast<uint32_t>(v0.b) << "," << static_cast<uint32_t>(v0.a) << ")"
                      << " rgba1=(" << static_cast<uint32_t>(v1.r) << "," << static_cast<uint32_t>(v1.g) << ","
                      << static_cast<uint32_t>(v1.b) << "," << static_cast<uint32_t>(v1.a) << ")"
                      << " texW=" << state.textureWidth << " texH=" << state.textureHeight
                      << " test=0x" << std::hex << ctx.test << " alpha=0x" << ctx.alpha << std::dec
                      << " texa=(aem=" << static_cast<uint32_t>(state.texa.aem)
                      << " ta0=0x" << std::hex << static_cast<uint32_t>(state.texa.ta0) << std::dec << ")"
                      << " tcc=" << static_cast<uint32_t>(ctx.tex0.tcc)
                      << " tfx=" << static_cast<uint32_t>(ctx.tex0.tfx) << std::endl;
    }

    const uint64_t alphaReg = ctx.alpha;
    const uint8_t alphaMode = static_cast<uint8_t>(alphaReg & 0xFFu);
    const uint8_t alphaFix = static_cast<uint8_t>((alphaReg >> 32) & 0xFFu);

    uint8_t r = v1.r, g = v1.g, b = v1.b, a = v1.a;

    if (state.prim.tme)
    {
        const auto &tex = ctx.tex0;
        const int texW = state.textureWidth;
        const int texH = state.textureHeight;

        float u0f, v0f, u1f, v1f;
        if (state.prim.fst)
        {
            u0f = static_cast<float>(v0.u >> 4);
            v0f = static_cast<float>(v0.v >> 4);
            u1f = static_cast<float>(v1.u >> 4);
            v1f = static_cast<float>(v1.v >> 4);
        }
        else
        {
            const float q0 = fabsQ(v0.q);
            const float q1 = fabsQ(v1.q);
            u0f = (v0.s / q0) * static_cast<float>(texW);
            v0f = (v0.t / q0) * static_cast<float>(texH);
            u1f = (v1.s / q1) * static_cast<float>(texW);
            v1f = (v1.t / q1) * static_cast<float>(texH);
        }

        float spriteW = static_cast<float>(spanX);
        float spriteH = static_cast<float>(spanY);
        if (spriteW < 1.0f)
            spriteW = 1.0f;
        if (spriteH < 1.0f)
            spriteH = 1.0f;

        for (int y = drawY0; y <= drawY1; ++y)
        {
            float ty = (static_cast<float>(y - unclippedY0) + 0.5f) / spriteH;
            float texVf = v0f + (v1f - v0f) * ty;

            for (int x = drawX0; x <= drawX1; ++x)
            {
                float tx = (static_cast<float>(x - unclippedX0) + 0.5f) / spriteW;
                float texUf = u0f + (u1f - u0f) * tx;
                uint32_t texel = 0xFFFF00FFu;
                if (state.prim.fst)
                {
                    const int fixedU = static_cast<int>((texUf * 16.0f) + 0.5f);
                    const int fixedV = static_cast<int>((texVf * 16.0f) + 0.5f);
                    const uint16_t sampleU = static_cast<uint16_t>(clampInt(fixedU, 0, 0xFFFF));
                    const uint16_t sampleV = static_cast<uint16_t>(clampInt(fixedV, 0, 0xFFFF));
                    texel = SampleTexture(state, 0.0f, 0.0f, 1.0f, sampleU, sampleV);
                }
                else
                {
                    texel = SampleTexture(state, texUf / static_cast<float>(texW), texVf / static_cast<float>(texH), 1.0f, 0u, 0u);
                }

                uint8_t tr = static_cast<uint8_t>(texel & 0xFF);
                uint8_t tg = static_cast<uint8_t>((texel >> 8) & 0xFF);
                uint8_t tb = static_cast<uint8_t>((texel >> 16) & 0xFF);
                uint8_t ta = static_cast<uint8_t>((texel >> 24) & 0xFF);

                const TextureCombineResult color = combineTexture(tex, r, g, b, a, tr, tg, tb, ta);
                WritePixel(state, x, y, z1, color.r, color.g, color.b, color.a, v1.fog);
            }
        }
    }
    else
    {
        for (int y = drawY0; y <= drawY1; ++y)
            for (int x = drawX0; x <= drawX1; ++x)
                WritePixel(state, x, y, z1, r, g, b, a, v1.fog);
    }
}

void GSCpuBackend::DrawTriangle(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    const GSVertex &v2 = batch.vertices[2];
    const auto &ctx = state.context;

    // DIAG 2026-10-18: la UV/ST del vertice que entra al draw texturizado (el texel sale (0,0) =>
    // su=0 sv=0 en todos los pixeles). Buscamos donde se pierde: packed (m_cur U/V o S/T/Q) o registro.
    {
        static uint32_t s_uvN = 0u;
        if (state.prim.tme && s_uvN++ < 6u)
            std::cout << "[uv-probe] tme=1 fst=" << static_cast<uint32_t>(state.prim.fst)
                      << " type=" << static_cast<uint32_t>(state.prim.type) << std::hex
                      << " v0(u=0x" << static_cast<uint32_t>(v0.u) << " v=0x" << static_cast<uint32_t>(v0.v) << ")"
                      << " v1(u=0x" << static_cast<uint32_t>(v1.u) << " v=0x" << static_cast<uint32_t>(v1.v) << ")"
                      << " v2(u=0x" << static_cast<uint32_t>(v2.u) << " v=0x" << static_cast<uint32_t>(v2.v) << ")"
                      << std::dec
                      << " s0=" << v0.s << " t0=" << v0.t << " q0=" << v0.q
                      << " s1=" << v1.s << " t1=" << v1.t << " q1=" << v1.q
                      << " s2=" << v2.s << " t2=" << v2.t << " q2=" << v2.q
                      << " xyz0=(" << v0.x << "," << v0.y << ")"
                      << " xyz1=(" << v1.x << "," << v1.y << ")"
                      << " xyz2=(" << v2.x << "," << v2.y << ")"
                      << " texW=" << state.textureWidth << " texH=" << state.textureHeight
                      << " tbp0=" << ctx.tex0.tbp0 << std::endl;
    }

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    float fx0 = v0.x - static_cast<float>(ofx);
    float fy0 = v0.y - static_cast<float>(ofy);
    float fx1 = v1.x - static_cast<float>(ofx);
    float fy1 = v1.y - static_cast<float>(ofy);
    float fx2 = v2.x - static_cast<float>(ofx);
    float fy2 = v2.y - static_cast<float>(ofy);

    int minX = static_cast<int>(std::floor(std::min({fx0, fx1, fx2})));
    int maxX = static_cast<int>(std::ceil(std::max({fx0, fx1, fx2})));
    int minY = static_cast<int>(std::floor(std::min({fy0, fy1, fy2})));
    int maxY = static_cast<int>(std::ceil(std::max({fy0, fy1, fy2})));

    minX = clampInt(minX, ctx.scissor.x0, ctx.scissor.x1);
    maxX = clampInt(maxX, ctx.scissor.x0, ctx.scissor.x1);
    minY = clampInt(minY, ctx.scissor.y0, ctx.scissor.y1);
    maxY = clampInt(maxY, ctx.scissor.y0, ctx.scissor.y1);

    float denom = (fy1 - fy2) * (fx0 - fx2) + (fx2 - fx1) * (fy0 - fy2);
    if (std::fabs(denom) < 0.001f)
        return;

    const float winding = (denom < 0.0f) ? -1.0f : 1.0f;
    const float invAbsDenom = 1.0f / std::fabs(denom);
    constexpr float kEdgeEpsilon = 1.0e-4f;
    // DIAG 2026-10-18 (Gemini v71 paso 1): loguear el PRIM QUE RAIZASTERIZA (una vez por prim, dentro del
    // lazo de pixeles), no los 6 primeros (que son padding degenerado con S=T=0).
    bool diagLoggedThisPrim = false;
    static uint32_t s_diagPrimLog = 0u;
    uint32_t diagTexelTotal = 0u, diagTexelNonZero = 0u;

    for (int y = minY; y <= maxY; ++y)
    {
        float py = static_cast<float>(y) + 0.5f;
        for (int x = minX; x <= maxX; ++x)
        {
            float px = static_cast<float>(x) + 0.5f;

            float w0 = (((fy1 - fy2) * (px - fx2) + (fx2 - fx1) * (py - fy2)) * winding) * invAbsDenom;
            float w1 = (((fy2 - fy0) * (px - fx2) + (fx0 - fx2) * (py - fy2)) * winding) * invAbsDenom;
            float w2 = 1.0f - w0 - w1;

            if (w0 < -kEdgeEpsilon || w1 < -kEdgeEpsilon || w2 < -kEdgeEpsilon)
                continue;

            double z = v0.z * w0 + v1.z * w1 + v2.z * w2;

            uint8_t r, g, b, a;
            if (state.prim.iip)
            {
                r = clampU8(static_cast<int>(v0.r * w0 + v1.r * w1 + v2.r * w2));
                g = clampU8(static_cast<int>(v0.g * w0 + v1.g * w1 + v2.g * w2));
                b = clampU8(static_cast<int>(v0.b * w0 + v1.b * w1 + v2.b * w2));
                a = clampU8(static_cast<int>(v0.a * w0 + v1.a * w1 + v2.a * w2));
            }
            else
            {
                r = v2.r;
                g = v2.g;
                b = v2.b;
                a = v2.a;
            }

            if (state.prim.tme)
            {
                float is, it, iq;
                uint16_t iu, iv;
                if (state.prim.fst)
                {
                    iu = static_cast<uint16_t>(v0.u * w0 + v1.u * w1 + v2.u * w2);
                    iv = static_cast<uint16_t>(v0.v * w0 + v1.v * w1 + v2.v * w2);
                    is = 0.0f;
                    it = 0.0f;
                    iq = 1.0f;
                }
                else
                {
                    // The GS DDA interpolates the homogeneous S, T and Q
                    // values. Texel coordinates are calculated from S/Q and
                    // T/Q only after interpolation.
                    is = v0.s * w0 + v1.s * w1 + v2.s * w2;
                    it = v0.t * w0 + v1.t * w1 + v2.t * w2;
                    iq = v0.q * w0 + v1.q * w1 + v2.q * w2;
                    iu = 0;
                    iv = 0;
                }

                uint32_t texel = SampleTexture(state, is, it, iq, iu, iv);

                uint8_t tr = static_cast<uint8_t>(texel & 0xFF);
                uint8_t tg = static_cast<uint8_t>((texel >> 8) & 0xFF);
                uint8_t tb = static_cast<uint8_t>((texel >> 16) & 0xFF);
                uint8_t ta = static_cast<uint8_t>((texel >> 24) & 0xFF);

                // DIAG 2026-10-18: histograma de texeles por prim (¿alguna muestra un glifo real?).
                ++diagTexelTotal;
                if (texel != 0u)
                    ++diagTexelNonZero;

                // DIAG 2026-10-18 (Gemini v76): estado de la CLUT en los draws del atlas (tbp0=10272).
                if (ctx.tex0.tbp0 == 10272u)
                {
                    static uint32_t s_glyphLog = 0u;
                    if (s_glyphLog++ < 8u)
                        std::cout << "[glyph-draw] cbp=" << ctx.tex0.cbp
                                  << " cld=" << static_cast<uint32_t>(ctx.tex0.cld)
                                  << " csm=" << static_cast<uint32_t>(ctx.tex0.csm)
                                  << " csa=" << static_cast<uint32_t>(ctx.tex0.csa)
                                  << " texel=0x" << std::hex << texel
                                  << " m_clut[1]=0x" << static_cast<uint32_t>(m_clut[1])
                                  << " m_clut[257]=0x" << static_cast<uint32_t>(m_clut[257])
                                  << " m_clut[15]=0x" << static_cast<uint32_t>(m_clut[15])
                                  << " m_clut[271]=0x" << static_cast<uint32_t>(m_clut[271])
                                  << std::dec << std::endl;
                }

                if (!diagLoggedThisPrim && s_diagPrimLog < 12u)
                {
                    ++s_diagPrimLog;
                    diagLoggedThisPrim = true;
                    std::cout << "[real-tri-probe] #" << s_diagPrimLog
                              << " fst=" << static_cast<uint32_t>(state.prim.fst)
                              << " type=" << static_cast<uint32_t>(state.prim.type)
                              << " fbp=" << ctx.frame.fbp << " fbw=" << ctx.frame.fbw
                              << " tbp0=" << ctx.tex0.tbp0 << " texW=" << state.textureWidth
                              << " texH=" << state.textureHeight
                              << " cbp=" << ctx.tex0.cbp << " cpsm=" << static_cast<uint32_t>(ctx.tex0.cpsm)
                              << " cld=" << static_cast<uint32_t>(ctx.tex0.cld)
                              << " csm=" << static_cast<uint32_t>(ctx.tex0.csm)
                              << " csa=" << static_cast<uint32_t>(ctx.tex0.csa)
                              << "\n  v0 xyz=(" << v0.x << "," << v0.y << ") st=(" << v0.s << "," << v0.t
                              << ") q=" << v0.q << " uv=(" << static_cast<uint32_t>(v0.u) << ","
                              << static_cast<uint32_t>(v0.v) << ") rgba=(" << static_cast<uint32_t>(v0.r)
                              << "," << static_cast<uint32_t>(v0.g) << "," << static_cast<uint32_t>(v0.b)
                              << "," << static_cast<uint32_t>(v0.a) << ")"
                              << "\n  v1 xyz=(" << v1.x << "," << v1.y << ") st=(" << v1.s << "," << v1.t
                              << ") q=" << v1.q
                              << "\n  v2 xyz=(" << v2.x << "," << v2.y << ") st=(" << v2.s << "," << v2.t
                              << ") q=" << v2.q
                              << "\n  px=(" << x << "," << y << ") is=" << is << " it=" << it
                              << " iq=" << iq << " iu=" << iu << " iv=" << iv
                              << " texUf=" << (state.prim.fst ? static_cast<float>(iu) / 16.0f
                                                              : is / fabsQ(iq) * static_cast<float>(state.textureWidth))
                              << " texVf=" << (state.prim.fst ? static_cast<float>(iv) / 16.0f
                                                              : it / fabsQ(iq) * static_cast<float>(state.textureHeight))
                              << " texel=0x" << std::hex << texel << std::dec
                              << " texrgba=(" << static_cast<uint32_t>(tr) << "," << static_cast<uint32_t>(tg)
                              << "," << static_cast<uint32_t>(tb) << "," << static_cast<uint32_t>(ta) << ")"
                              << std::endl;
                }

                const auto &tex = ctx.tex0;
                const uint8_t shadeR = r;
                const uint8_t shadeG = g;
                const uint8_t shadeB = b;
                const uint8_t shadeA = a;
                const TextureCombineResult color = combineTexture(tex, shadeR, shadeG, shadeB, shadeA, tr, tg, tb, ta);

                r = color.r;
                g = color.g;
                b = color.b;
                a = color.a;
                // DIAG 2026-10-18: separar vertice vs texel como fuente del alpha=0 (que hace que el
                // alpha test, ATST=5 AREF=2 AFAIL=KEEP, descarte TODO y la VRAM quede vacia).
                {
                    static uint32_t s_tpN = 0u;
                    if (s_tpN++ < 8u)
                        std::cout << "[tri-probe] v0=(" << static_cast<uint32_t>(v0.r) << "," << static_cast<uint32_t>(v0.g)
                                  << "," << static_cast<uint32_t>(v0.b) << "," << static_cast<uint32_t>(v0.a) << ")"
                                  << " shade=(" << static_cast<uint32_t>(shadeR) << "," << static_cast<uint32_t>(shadeG)
                                  << "," << static_cast<uint32_t>(shadeB) << "," << static_cast<uint32_t>(shadeA) << ")"
                                  << " texel=(" << static_cast<uint32_t>(tr) << "," << static_cast<uint32_t>(tg)
                                  << "," << static_cast<uint32_t>(tb) << "," << static_cast<uint32_t>(ta) << ")"
                                  << " out=(" << static_cast<uint32_t>(color.r) << "," << static_cast<uint32_t>(color.g)
                                  << "," << static_cast<uint32_t>(color.b) << "," << static_cast<uint32_t>(color.a) << ")"
                                  << " tcc=" << static_cast<uint32_t>(tex.tcc) << " tfx=" << static_cast<uint32_t>(tex.tfx)
                                  << " psm=" << static_cast<uint32_t>(tex.psm) << " cpsm=" << static_cast<uint32_t>(tex.cpsm)
                                  << " tbp0=" << tex.tbp0 << " cbp=" << tex.cbp
                                  << " cld=" << static_cast<uint32_t>(tex.cld)
                                  << " csm=" << static_cast<uint32_t>(tex.csm)
                                  << " csa=" << static_cast<uint32_t>(tex.csa)
                                  << " iip=" << static_cast<uint32_t>(state.prim.iip)
                                  << " xy=(" << x << "," << y << ")" << std::endl;
                }
            }

            const uint8_t fog = clampU8(static_cast<int>(v0.fog * w0 + v1.fog * w1 + v2.fog * w2));
            WritePixel(state, x, y, static_cast<u32>(z + 0.5), r, g, b, a, fog);
        }
    }

    // DIAG 2026-10-18: resumen por prim (¿los prims texturizados muestrean glifos reales alguna vez?).
    if (state.prim.tme)
    {
        static uint32_t s_histN = 0u, s_primTotal = 0u, s_primWithNonZero = 0u;
        static uint32_t s_stVaryPrims = 0u, s_stVaryTexelNZ = 0u;
        ++s_primTotal;
        if (diagTexelNonZero != 0u)
            ++s_primWithNonZero;
        const bool stVary = (v0.s != v1.s) || (v1.s != v2.s) || (v0.t != v1.t) || (v1.t != v2.t);
        if (stVary)
        {
            ++s_stVaryPrims;
            if (diagTexelNonZero != 0u)
                ++s_stVaryTexelNZ;
        }
        if (s_histN < 20u)
        {
            ++s_histN;
            std::cout << "[tex-hist] #" << s_histN
                      << " fbp=" << ctx.frame.fbp
                      << " tbp0=" << ctx.tex0.tbp0
                      << " psm=" << static_cast<uint32_t>(ctx.tex0.psm)
                      << " rgba0=(" << static_cast<uint32_t>(v0.r) << "," << static_cast<uint32_t>(v0.g)
                      << "," << static_cast<uint32_t>(v0.b) << "," << static_cast<uint32_t>(v0.a) << ")"
                      << " stvary=" << (stVary ? 1 : 0)
                      << " px=" << diagTexelTotal << " texelNZ=" << diagTexelNonZero
                      << std::endl;
        }
        if ((s_primTotal % 200u) == 0u)
            std::cout << "[tex-hist] TOTAL texturedPrims=" << s_primTotal
                      << " withNonZeroTexel=" << s_primWithNonZero
                      << " stVaryPrims=" << s_stVaryPrims
                      << " stVaryWithNonZeroTexel=" << s_stVaryTexelNZ << std::endl;
    }
}

void GSCpuBackend::DrawLine(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    const auto &ctx = state.context;

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    int x0 = static_cast<int>(v0.x) - ofx;
    int y0 = static_cast<int>(v0.y) - ofy;
    int x1 = static_cast<int>(v1.x) - ofx;
    int y1 = static_cast<int>(v1.y) - ofy;

    int dx = std::abs(x1 - x0);
    int dy = -std::abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    int totalSteps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
    if (totalSteps == 0)
        totalSteps = 1;
    int step = 0;

    for (;;)
    {
        float t = static_cast<float>(step) / static_cast<float>(totalSteps);
        uint8_t r, g, b, a;
        if (state.prim.iip)
        {
            r = clampU8(static_cast<int>(v0.r + (v1.r - v0.r) * t));
            g = clampU8(static_cast<int>(v0.g + (v1.g - v0.g) * t));
            b = clampU8(static_cast<int>(v0.b + (v1.b - v0.b) * t));
            a = clampU8(static_cast<int>(v0.a + (v1.a - v0.a) * t));
        }
        else
        {
            r = v1.r;
            g = v1.g;
            b = v1.b;
            a = v1.a;
        }

        double z = (v0.z + (v1.z - v0.z) * t);
        const uint8_t fog = clampU8(static_cast<int>(v0.fog + (v1.fog - v0.fog) * t));
        WritePixel(state, x0, y0, static_cast<u32>(z), r, g, b, a, fog);

        if (x0 == x1 && y0 == y1)
            break;

        int e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
        ++step;
    }
}

void GSCpuBackend::BeginTransfer(const GSTransferCommand &command)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_transfer = command;
    m_transferState.x = command.trxpos.dsax;
    m_transferState.y = command.trxpos.dsay;
    m_transferState.totalPixels = static_cast<uint32_t>(command.trxreg.rrw) * static_cast<uint32_t>(command.trxreg.rrh);
    m_transferState.copiedPixels = 0u;
    m_transferState.direction = command.direction;
    m_transferState.localToHostPendingBytes = 0u;

    if (command.direction == 2u)
        PerformLocalToLocalTransfer();
    else if (command.direction == 1u)
        PerformLocalToHostTransfer();
}

void GSCpuBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!data || sizeBytes == 0u || !m_vram || m_transferState.direction != 0u)
        return;
    if (m_transfer.trxreg.rrw == 0u || m_transfer.trxreg.rrh == 0u || m_transferState.totalPixels == 0u)
        return;

    const uint32_t dbp = m_transfer.bitbltbuf.dbp;
    const uint32_t dbw = std::max<uint32_t>(m_transfer.bitbltbuf.dbw, 1u);
    const uint8_t dpsm = m_transfer.bitbltbuf.dpsm;
    const uint32_t rrw = m_transfer.trxreg.rrw;
    const uint32_t dsax = m_transfer.trxpos.dsax;
    uint32_t offset = 0u;

    auto advancePixel = [&](uint32_t count)
    {
        const uint32_t totalPixels = m_transferState.totalPixels;
        m_transferState.copiedPixels =
            std::min<uint32_t>(totalPixels, m_transferState.copiedPixels + count);

        if (m_transferState.copiedPixels >= totalPixels)
        {
            m_transferState.direction = 3u;
            m_transferState.totalPixels = 0u;
            return;
        }

        m_transferState.x = dsax + (m_transferState.copiedPixels % rrw);
        m_transferState.y = m_transfer.trxpos.dsay + (m_transferState.copiedPixels / rrw);
    };

    while (offset < sizeBytes && m_transferState.direction == 0u)
    {
        switch (dpsm)
        {
        case GS_PSM_CT32:
        case GS_PSM_Z32:
        {
            if (sizeBytes - offset < 4u)
                return;
            uint32_t value = 0u;
            std::memcpy(&value, data + offset, sizeof(value));
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, value);
            offset += 4u;
            advancePixel(1u);
            break;
        }
        case GS_PSM_CT24:
        case GS_PSM_Z24:
        {
            if (sizeBytes - offset < 3u)
                return;
            const uint32_t value = static_cast<uint32_t>(data[offset]) |
                                   (static_cast<uint32_t>(data[offset + 1u]) << 8u) |
                                   (static_cast<uint32_t>(data[offset + 2u]) << 16u);
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, value);
            offset += 3u;
            advancePixel(1u);
            break;
        }
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
        {
            if (sizeBytes - offset < 2u)
                return;
            uint16_t value = 0u;
            std::memcpy(&value, data + offset, sizeof(value));
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, value);
            offset += 2u;
            advancePixel(1u);
            break;
        }
        case GS_PSM_T8:
        case GS_PSM_T8H:
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, data[offset++]);
            advancePixel(1u);
            break;
        case GS_PSM_T4:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
        {
            const uint8_t packed = data[offset++];
            const uint32_t firstPixel = m_transferState.copiedPixels;
            WriteVramUnlocked(dpsm, dbp, dbw,
                              dsax + (firstPixel % rrw),
                              m_transfer.trxpos.dsay + (firstPixel / rrw),
                              packed & 0x0Fu);
            if (firstPixel + 1u < m_transferState.totalPixels)
            {
                const uint32_t secondPixel = firstPixel + 1u;
                WriteVramUnlocked(dpsm, dbp, dbw,
                                  dsax + (secondPixel % rrw),
                                  m_transfer.trxpos.dsay + (secondPixel / rrw),
                                  (packed >> 4u) & 0x0Fu);
            }
            advancePixel(std::min<uint32_t>(2u, m_transferState.totalPixels - firstPixel));
            break;
        }
        default:
            return;
        }
    }
}

void GSCpuBackend::PerformLocalToLocalTransfer()
{
    if (!m_vram)
        return;

    const uint32_t rrw = m_transfer.trxreg.rrw;
    const uint32_t rrh = m_transfer.trxreg.rrh;
    const uint32_t total = rrw * rrh;
    if (total == 0u)
    {
        m_transferState.direction = 3u;
        return;
    }

    for (uint32_t pixel = 0; pixel < total; ++pixel)
    {
        uint32_t x = pixel % rrw;
        uint32_t y = pixel / rrw;
        if ((m_transfer.trxpos.dir & 0x2u) != 0u)
            x = rrw - x - 1u;
        if ((m_transfer.trxpos.dir & 0x1u) != 0u)
            y = rrh - y - 1u;

        const uint32_t value = ReadVramUnlocked(m_transfer.bitbltbuf.spsm,
                                                m_transfer.bitbltbuf.sbp,
                                                std::max<uint32_t>(m_transfer.bitbltbuf.sbw, 1u),
                                                x + m_transfer.trxpos.ssax,
                                                y + m_transfer.trxpos.ssay);
        WriteVramUnlocked(m_transfer.bitbltbuf.dpsm,
                          m_transfer.bitbltbuf.dbp,
                          std::max<uint32_t>(m_transfer.bitbltbuf.dbw, 1u),
                          x + m_transfer.trxpos.dsax,
                          y + m_transfer.trxpos.dsay,
                          value);
    }

    m_transferState.copiedPixels = total;
    m_transferState.direction = 3u;
}

void GSCpuBackend::PerformLocalToHostTransfer()
{
    m_localToHostBuffer.clear();
    m_localToHostReadPos = 0u;
    if (!m_vram)
        return;

    const uint32_t rrw = m_transfer.trxreg.rrw;
    const uint32_t rrh = m_transfer.trxreg.rrh;
    const uint32_t sbw = std::max<uint32_t>(m_transfer.bitbltbuf.sbw, 1u);
    const uint8_t spsm = m_transfer.bitbltbuf.spsm;
    const uint32_t bpp = static_cast<uint32_t>(GSMem::BitsPerPixel(static_cast<GSMem::PixelStorageMode>(spsm)));
    const uint32_t total = rrw * rrh;
    m_localToHostBuffer.reserve((static_cast<size_t>(total) * bpp + 7u) / 8u);

    for (uint32_t pixel = 0u; pixel < total; ++pixel)
    {
        const uint32_t x = pixel % rrw;
        const uint32_t y = pixel / rrw;
        const uint32_t value = ReadVramUnlocked(spsm,
                                                m_transfer.bitbltbuf.sbp,
                                                sbw,
                                                x + m_transfer.trxpos.ssax,
                                                y + m_transfer.trxpos.ssay);
        switch (bpp)
        {
        case 32:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 8u));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 16u));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 24u));
            break;
        case 24:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 8u));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 16u));
            break;
        case 16:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 8u));
            break;
        case 8:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            break;
        case 4:
        {
            if ((pixel & 1u) != 0u)
                break;
            uint32_t next = 0u;
            if (pixel + 1u < total)
            {
                const uint32_t nextPixel = pixel + 1u;
                const uint32_t nextX = nextPixel % rrw;
                const uint32_t nextY = nextPixel / rrw;
                next = ReadVramUnlocked(spsm, m_transfer.bitbltbuf.sbp, sbw,
                                        nextX + m_transfer.trxpos.ssax,
                                        nextY + m_transfer.trxpos.ssay);
            }
            m_localToHostBuffer.push_back(static_cast<uint8_t>((value & 0x0Fu) | ((next & 0x0Fu) << 4u)));
            break;
        }
        default:
            break;
        }
    }

    m_transferState.copiedPixels = total;
    m_transferState.localToHostPendingBytes = m_localToHostBuffer.size();
}

uint32_t GSCpuBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!dst || maxBytes == 0u || m_localToHostReadPos >= m_localToHostBuffer.size())
        return 0u;
    const size_t count = std::min<size_t>(maxBytes, m_localToHostBuffer.size() - m_localToHostReadPos);
    std::memcpy(dst, m_localToHostBuffer.data() + m_localToHostReadPos, count);
    m_localToHostReadPos += count;
    m_transferState.localToHostPendingBytes = m_localToHostBuffer.size() - m_localToHostReadPos;
    return static_cast<uint32_t>(count);
}

bool GSCpuBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || context.frame.fbw == 0u)
        return false;

    const uint32_t x0 = context.scissor.x0;
    const uint32_t x1 = std::max<uint32_t>(x0, context.scissor.x1);
    const uint32_t y0 = context.scissor.y0;
    const uint32_t y1 = std::max<uint32_t>(y0, context.scissor.y1);
    uint8_t r = static_cast<uint8_t>(rgba);
    uint8_t g = static_cast<uint8_t>(rgba >> 8u);
    uint8_t b = static_cast<uint8_t>(rgba >> 16u);
    uint8_t a = static_cast<uint8_t>(rgba >> 24u);
    if ((context.fba & 1ull) != 0ull && context.frame.psm != GS_PSM_CT24)
        a |= 0x80u;

    const uint32_t fbp = GSInternal::framePageBaseToBlock(context.frame.fbp);
    const uint32_t fbw = std::max<uint32_t>(context.frame.fbw, 1u);
    if (context.frame.psm == GS_PSM_CT32 || context.frame.psm == GS_PSM_CT24)
    {
        const uint32_t source = static_cast<uint32_t>(r) |
                                (static_cast<uint32_t>(g) << 8u) |
                                (static_cast<uint32_t>(b) << 16u) |
                                (static_cast<uint32_t>(a) << 24u);
        for (uint32_t y = y0; y <= y1; ++y)
            for (uint32_t x = x0; x <= x1; ++x)
            {
                uint32_t pixel = source;
                if (context.frame.fbmsk != 0u)
                {
                    const uint32_t old = ReadVramUnlocked(context.frame.psm, fbp, fbw, x, y);
                    pixel = (pixel & ~context.frame.fbmsk) | (old & context.frame.fbmsk);
                }
                WriteVramUnlocked(context.frame.psm, fbp, fbw, x, y, pixel);
            }
        return true;
    }

    if (context.frame.psm == GS_PSM_CT16 || context.frame.psm == GS_PSM_CT16S)
    {
        const uint16_t source = encodeFramePixelPSMCT16(r, g, b, a);
        const uint16_t mask = static_cast<uint16_t>(context.frame.fbmsk);
        for (uint32_t y = y0; y <= y1; ++y)
            for (uint32_t x = x0; x <= x1; ++x)
            {
                uint16_t pixel = source;
                if (mask != 0u)
                {
                    const uint16_t old = static_cast<uint16_t>(ReadVramUnlocked(context.frame.psm, fbp, fbw, x, y));
                    pixel = static_cast<uint16_t>((pixel & ~mask) | (old & mask));
                }
                WriteVramUnlocked(context.frame.psm, fbp, fbw, x, y, pixel);
            }
        return true;
    }
    return false;
}

bool GSCpuBackend::CopyFrameToHostRgba(const GSFrameReg &frame,
                                       uint32_t width,
                                       uint32_t height,
                                       std::vector<uint8_t> &outPixels,
                                       bool preserveAlpha,
                                       bool useLocalMemoryLayout,
                                       bool frameBaseIsPages,
                                       uint32_t sourceOriginX,
                                       uint32_t sourceOriginY) const
{
    if (!m_vram || m_vramSize == 0u)
        return false;

    outPixels.assign(kHostFrameWidth * kHostFrameHeight * 4u, 0u);
    const uint32_t baseBytes = frameBaseIsPages ? frame.fbp * 8192u : frame.fbp * 256u;
    const uint32_t basePtr = frameBaseIsPages ? GSInternal::framePageBaseToBlock(frame.fbp) : frame.fbp;
    const uint32_t fbw = frame.fbw ? frame.fbw : kHostFrameWidth / 64u;
    const uint32_t bytesPerPixel = (frame.psm == GS_PSM_CT16 || frame.psm == GS_PSM_CT16S) ? 2u : 4u;
    const uint32_t stride = fbw * 64u * bytesPerPixel;

    for (uint32_t y = 0; y < height; ++y)
    {
        uint8_t *dst = outPixels.data() + y * kHostFrameWidth * 4u;
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint32_t sx = sourceOriginX + x;
            const uint32_t sy = sourceOriginY + y;
            if (frame.psm == GS_PSM_CT32 || frame.psm == GS_PSM_CT24)
            {
                uint32_t color = 0u;
                if (useLocalMemoryLayout)
                    color = ReadVramUnlocked(frame.psm, basePtr, fbw, sx, sy);
                else
                {
                    const uint32_t pixelBytes = frame.psm == GS_PSM_CT24 ? 3u : 4u;
                    const uint64_t offset = static_cast<uint64_t>(baseBytes) + static_cast<uint64_t>(sy) * stride + static_cast<uint64_t>(sx) * pixelBytes;
                    if (offset + pixelBytes > m_vramSize)
                        return false;
                    color = m_vram[offset] | (static_cast<uint32_t>(m_vram[offset + 1u]) << 8u) |
                            (static_cast<uint32_t>(m_vram[offset + 2u]) << 16u);
                    if (pixelBytes == 4u)
                        color |= static_cast<uint32_t>(m_vram[offset + 3u]) << 24u;
                }
                dst[x * 4u] = static_cast<uint8_t>(color);
                dst[x * 4u + 1u] = static_cast<uint8_t>(color >> 8u);
                dst[x * 4u + 2u] = static_cast<uint8_t>(color >> 16u);
                dst[x * 4u + 3u] = preserveAlpha && frame.psm != GS_PSM_CT24 ? static_cast<uint8_t>(color >> 24u) : 255u;
            }
            else if (frame.psm == GS_PSM_CT16 || frame.psm == GS_PSM_CT16S)
            {
                uint16_t color = 0u;
                if (useLocalMemoryLayout)
                    color = static_cast<uint16_t>(ReadVramUnlocked(frame.psm, basePtr, fbw, sx, sy));
                else
                {
                    const uint64_t offset = static_cast<uint64_t>(baseBytes) + static_cast<uint64_t>(sy) * stride + static_cast<uint64_t>(sx) * 2u;
                    if (offset + 2u > m_vramSize)
                        return false;
                    std::memcpy(&color, m_vram + offset, sizeof(color));
                }
                const uint32_t r = color & 31u;
                const uint32_t g = (color >> 5u) & 31u;
                const uint32_t b = (color >> 10u) & 31u;
                dst[x * 4u] = static_cast<uint8_t>((r << 3u) | (r >> 2u));
                dst[x * 4u + 1u] = static_cast<uint8_t>((g << 3u) | (g >> 2u));
                dst[x * 4u + 2u] = static_cast<uint8_t>((b << 3u) | (b >> 2u));
                dst[x * 4u + 3u] = preserveAlpha ? ((color & 0x8000u) ? 0x80u : 0u) : 255u;
            }
            else
            {
                outPixels.clear();
                return false;
            }
        }
    }
    return true;
}

PresentationFrame GSCpuBackend::Present(const GSPresentationRequest &request)
{
    // 2026-10-10 DIAG: volcar el VRAM del recomp una vez (para diffear contra el real).
    // Env: SWORD_DUMP_VRAM=1 → /tmp/recomp_vram.bin (4 MB) al present #1500.
    if (const char *vp = std::getenv("SWORD_DUMP_VRAM"); vp != nullptr && *vp != '\0')
    {
        static uint32_t s_presentN = 0u;
        const uint32_t n = ++s_presentN;
        static const uint32_t s_dumpAt = []() {
            const char *a = std::getenv("SWORD_DUMP_VRAM_AT");
            return (a != nullptr && *a != '\0') ? static_cast<uint32_t>(std::strtoul(a, nullptr, 10)) : 0u;
        }();
        if (n == 300u || n == 900u || n == 1500u || (s_dumpAt != 0u && n == s_dumpAt))
        {
            const std::string path = std::strcmp(vp, "1") == 0 ? std::string("/tmp/recomp_vram.bin") : std::string(vp);
            std::ofstream out(path, std::ios::binary);
            if (out && m_vram != nullptr && m_vramSize != 0u)
                out.write(reinterpret_cast<const char *>(m_vram), m_vramSize);
            std::cout << "[dump-vram] present=" << n << " size=" << m_vramSize << " path=" << path << std::endl;
        }
    }

    // 2026-10-10 DIAG: patrón de prueba forzado para validar el camino de presentación
    // (GS backend -> host frame -> pantalla), independiente de lo que dibuje el juego.
    // Env: SWORD_TEST_PATTERN=1. Devuelve un gradiente con "HELLO WORLD" en amarillo.
    if (const char *tp = std::getenv("SWORD_TEST_PATTERN"); tp != nullptr && *tp != '\0')
    {
        static const uint8_t kGlyphs[8][7] = {
            {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, // H
            {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, // E
            {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}, // L
            {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, // O
            {0x11, 0x11, 0x11, 0x11, 0x11, 0x15, 0x0A}, // W
            {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}, // R
            {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}, // D
            {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // (space)
        };
        static const int kMsg[] = {0, 1, 2, 2, 3, 7, 4, 3, 5, 2, 6}; // HELLO WORLD
        PresentationFrame pat{};
        pat.width = kHostFrameWidth;
        pat.height = kHostFrameHeight;
        pat.pixels.assign(static_cast<size_t>(kHostFrameWidth) * kHostFrameHeight * 4u, 0u);
        for (uint32_t y = 0; y < kHostFrameHeight; ++y)
            for (uint32_t x = 0; x < kHostFrameWidth; ++x)
            {
                uint8_t *p = pat.pixels.data() + (static_cast<size_t>(y) * kHostFrameWidth + x) * 4u;
                p[0] = static_cast<uint8_t>(x * 255u / (kHostFrameWidth - 1u));
                p[1] = static_cast<uint8_t>(y * 255u / (kHostFrameHeight - 1u));
                p[2] = 64u;
                p[3] = 255u;
            }
        constexpr int scale = 8, gw = 5, gh = 7, gap = 1;
        const int textW = static_cast<int>(sizeof(kMsg) / sizeof(kMsg[0])) * (gw + gap) * scale;
        const int ox = static_cast<int>(kHostFrameWidth) / 2 - textW / 2;
        const int oy = static_cast<int>(kHostFrameHeight) / 2 - gh * scale / 2;
        for (size_t c = 0; c < sizeof(kMsg) / sizeof(kMsg[0]); ++c)
        {
            const uint8_t *g = kGlyphs[kMsg[c]];
            for (int ry = 0; ry < gh; ++ry)
                for (int rx = 0; rx < gw; ++rx)
                    if ((g[ry] & (1u << (gw - 1 - rx))) != 0u)
                        for (int sy = 0; sy < scale; ++sy)
                            for (int sx = 0; sx < scale; ++sx)
                            {
                                const int px = ox + static_cast<int>(c) * (gw + gap) * scale + rx * scale + sx;
                                const int py = oy + ry * scale + sy;
                                if (px >= 0 && px < static_cast<int>(kHostFrameWidth) &&
                                    py >= 0 && py < static_cast<int>(kHostFrameHeight))
                                {
                                    uint8_t *p = pat.pixels.data() +
                                                 (static_cast<size_t>(py) * kHostFrameWidth + static_cast<uint32_t>(px)) * 4u;
                                    p[0] = 255u;
                                    p[1] = 255u;
                                    p[2] = 0u;
                                    p[3] = 255u;
                                }
                            }
        }
        static bool s_logged = false;
        if (!s_logged)
        {
            s_logged = true;
            std::cout << "[test-pattern] HELLO WORLD " << kHostFrameWidth << "x" << kHostFrameHeight << std::endl;
        }
        return pat;
    }

    // Snapshot local memory under the backend lock, then perform the expensive
    // display conversion without holding the producer-side raster lock.
    // DIAG 2026-10-18 (Gemini v78): inyectar la page 321 REAL (atlas de glifos + su paleta) en cada
    // present. Necesario porque el juego re-sube la page 320 en dbp=10272 (page 321 queda como copia).
    // Env-gated: SWORD_INJECT_PAGE321=<GS sin header>. NO es fix real.
    if (const char *pagePath = std::getenv("SWORD_INJECT_PAGE321"); pagePath != nullptr && m_vram != nullptr)
    {
        static std::vector<uint8_t> s_page321;
        static bool s_page321Loaded = false;
        if (!s_page321Loaded)
        {
            s_page321Loaded = true;
            if (std::FILE *pf = std::fopen(pagePath, "rb"))
            {
                s_page321.resize(0x2000u);
                std::fseek(pf, 0x282000L, SEEK_SET);
                if (std::fread(s_page321.data(), 1u, 0x2000u, pf) != 0x2000u)
                    s_page321.clear();
                std::fclose(pf);
                std::cout << "[inject-page321] loaded=" << s_page321.size() << " from " << pagePath << std::endl;
            }
        }
        if (s_page321.size() == 0x2000u && m_vramSize >= 0x284000u)
            std::memcpy(m_vram + 0x282000u, s_page321.data(), 0x2000u);
    }

    // DIAG 2026-10-18 (Gemini v80): inyectar las 11 paginas de UI (0x280000..0x296000, 88 KB) e
    // invalidar caches. Env-gated: SWORD_INJECT_UI_VRAM=<GS sin header>. NO es fix real.
    if (const char *uiPath = std::getenv("SWORD_INJECT_UI_VRAM"); uiPath != nullptr && m_vram != nullptr)
    {
        static std::vector<uint8_t> s_uiVram;
        static bool s_uiVramLoaded = false;
        static uint32_t s_uiVramPresent = 0u;
        if (!s_uiVramLoaded)
        {
            s_uiVramLoaded = true;
            if (std::FILE *pf = std::fopen(uiPath, "rb"))
            {
                s_uiVram.resize(0x16000u);
                std::fseek(pf, 0x280000L, SEEK_SET);
                if (std::fread(s_uiVram.data(), 1u, 0x16000u, pf) != 0x16000u)
                    s_uiVram.clear();
                std::fclose(pf);
                std::cout << "[inject-ui-vram] loaded=" << s_uiVram.size() << " from " << uiPath << std::endl;
            }
        }
        if (s_uiVram.size() == 0x16000u && m_vramSize >= 0x296000u)
        {
            std::memcpy(m_vram + 0x280000u, s_uiVram.data(), 0x16000u);
            m_texturePageCache.Invalidate();
            m_clutCbp.fill(0xFFFFFFFFu);
            if (++s_uiVramPresent <= 3u)
            {
                std::cout << "[inject-ui-vram] copiadas 11 paginas + caches invalidados (present #"
                          << s_uiVramPresent << ")" << std::endl;
            }
        }
    }

    thread_local std::vector<uint8_t> snapshot;
    SnapshotVram(snapshot);
    if (snapshot.empty())
        return {};

    thread_local GSCpuBackend snapshotBackend;
    snapshotBackend.Initialize(snapshot.data(), static_cast<uint32_t>(snapshot.size()));
    return snapshotBackend.PresentFromLocalMemory(request);
}

PresentationFrame GSCpuBackend::PresentFromLocalMemory(const GSPresentationRequest &request)
{
    PresentationFrame result{};
    const GSPmodeState pmode = decodePmode(request.pmode);
    const GSSmode2State smode2 = decodeSMode2(request.smode2);
    const bool fieldMode = smode2.interlaced && !smode2.frameMode;
    const bool oddField = (request.vsyncTick & 1ull) != 0ull;
    GSFrameReg displayFrame1 = decodeDisplayFrame(request.dispfb1);
    const GSFrameReg displayFrame2 = decodeDisplayFrame(request.dispfb2);
    // DIAG 2026-10-18 (Gemini v71 paso 3): el juego NUNCA escribe DISPFB1 (queda en el default del
    // runtime 0x1400 = fbp 0) pero dibuja en el frame del contexto activo (page 128, que es el DISPFB1
    // del real). Env-gated: si dispfb1 es el default, presentar el frame del contexto 0.
    if (std::getenv("SWORD_PRESENT_CTX") != nullptr && request.dispfb1 == 0x1400ull &&
        request.contextFrames[0].fbw != 0u)
    {
        static bool s_ctxLog = false;
        if (!s_ctxLog)
        {
            s_ctxLog = true;
            std::cout << "[present-ctx] dispfb1 default -> presentando ctx frame fbp="
                      << request.contextFrames[0].fbp << " fbw=" << request.contextFrames[0].fbw
                      << " psm=" << static_cast<uint32_t>(request.contextFrames[0].psm) << std::endl;
        }
        displayFrame1 = request.contextFrames[0];
    }
    const GSDisplayReadOrigin origin1 = decodeDisplayReadOrigin(request.dispfb1);
    const GSDisplayReadOrigin origin2 = decodeDisplayReadOrigin(request.dispfb2);
    uint32_t width1 = 0u, height1 = 0u, width2 = 0u, height2 = 0u;
    decodeDisplaySize(request.display1, width1, height1);
    decodeDisplaySize(request.display2, width2, height2);
    const bool valid1 = pmode.enableCrt1 && hasDisplaySetup(request.display1, displayFrame1) &&
                        !(std::getenv("SWORD_NO_CRT1_DEFAULT") != nullptr &&
                          request.dispfb1 == ((0ULL << 0) | (10ULL << 9) | (0ULL << 15) | (0ULL << 32) | (0ULL << 43)));
    const bool valid2 = pmode.enableCrt2 && hasDisplaySetup(request.display2, displayFrame2);
    static uint32_t s_presentLogCount = 0;
    s_presentLogCount++;
    if (s_presentLogCount < 10 || (s_presentLogCount % 10) == 0)
    {
        // 2026-10-17: ademas del total, histograma de VRAM por cuarto de MB (16 buckets de 256 KB) y
        // los fbp de display y del contexto de dibujo. Hace falta para responder "donde esta el
        // contenido": con vramNonZero=475456 el diag dice `crt1 (fbp=0): rgbNZ=0` => se presenta una
        // pagina vacia mientras hay data en otra.
        uint32_t totalNonZero = 0;
        uint32_t bucket[16] = {0};
        for (size_t i = 0; i < m_vramSize; ++i) {
            if (m_vram[i] != 0) {
                totalNonZero++;
                bucket[(i >> 18) & 0xFu]++;
            }
        }
        std::cout << "[gs:present] #" << s_presentLogCount
                  << " valid1=" << valid1 << " valid2=" << valid2
                  << " pmode=0x" << std::hex << request.pmode
                  << " dispfb1=0x" << request.dispfb1
                  << " display1=0x" << request.display1
                  << " dispfb2=0x" << request.dispfb2
                  << " display2=0x" << request.display2
                  << std::dec << " vramNonZero=" << totalNonZero << "/" << m_vramSize
                  << " fbpDisp1=" << displayFrame1.fbp << " fbw1=" << displayFrame1.fbw
                  << " psm1=" << static_cast<uint32_t>(displayFrame1.psm)
                  << " fbpDisp2=" << displayFrame2.fbp << " ctx=";
        for (const GSFrameReg &cf : request.contextFrames)
            std::cout << std::hex << cf.fbp << std::dec << "/" << cf.fbw << "/" << static_cast<uint32_t>(cf.psm) << " ";
        std::cout << " per256K=";
        for (uint32_t b = 0; b < 16u; ++b)
            if (bucket[b] != 0u)
                std::cout << "[" << b << "]=" << bucket[b] << " ";
        std::cout << std::endl;
    }
    if (!valid1 && !valid2)
        return result;

    static uint32_t s_pixelDiagCount = 0;
    const bool logDiag = (s_pixelDiagCount++ % 20) == 0;

    auto copySource = [&](const GSFrameReg &displayFrame,
                          const GSDisplayReadOrigin &origin,
                          uint32_t width,
                          uint32_t height,
                          bool allowPreferred,
                          bool preserveAlpha,
                          GSFrameReg &selected,
                          std::vector<uint8_t> &pixels,
                          bool &usedPreferred) -> bool
    {
        selected = displayFrame;
        pixels.clear();
        usedPreferred = false;
        if (allowPreferred && request.hasPreferredSource && request.preferredDestFbp == displayFrame.fbp &&
            (request.preferredSource.fbw != 0u || request.preferredSource.fbp != displayFrame.fbp) &&
            CopyFrameToHostRgba(request.preferredSource, width, height, pixels, preserveAlpha, true, false, 0u, 0u))
        {
            selected = request.preferredSource;
            usedPreferred = true;
        }
        if (pixels.empty() && !CopyFrameToHostRgba(displayFrame, width, height, pixels, preserveAlpha, true, true, origin.x, origin.y))
            return false;

        if (!usedPreferred && displayFrame.fbp == 0u && countNonBlackPixels(pixels, width, height) == 0u)
        {
            static uint32_t s_fbLog = 0u;
            const bool logFb = (s_fbLog++ < 8u);
            for (const GSFrameReg &candidate : request.contextFrames)
            {
                if (candidate.fbp == selected.fbp && candidate.fbw == selected.fbw && candidate.psm == selected.psm)
                {
                    if (logFb)
                        std::cout << "[present-fb] skip cand fbp=" << candidate.fbp << " (== selected)" << std::endl;
                    continue;
                }
                std::vector<uint8_t> candidatePixels;
                const bool ok = CopyFrameToHostRgba(candidate, width, height, candidatePixels, preserveAlpha, true, true, 0u, 0u);
                const uint32_t cnz = ok ? countNonBlackPixels(candidatePixels, width, height) : 0u;
                if (logFb)
                    std::cout << "[present-fb] cand fbp=" << candidate.fbp << " fbw=" << candidate.fbw
                              << " psm=" << static_cast<uint32_t>(candidate.psm) << " copy=" << ok
                              << " nz=" << cnz << " (w=" << width << " h=" << height << ")" << std::endl;
                if (!ok)
                    continue;
                if (cnz == 0u)
                    continue;
                selected = candidate;
                pixels.swap(candidatePixels);
                break;
            }
            if (logFb)
                std::cout << "[present-fb] -> selected.fbp=" << selected.fbp
                          << " hasPreferred=" << request.hasPreferredSource
                          << " prefSrc.fbp=" << request.preferredSource.fbp
                          << " prefDestFbp=" << request.preferredDestFbp << std::endl;
        }
        return true;
    };

    if (valid1 && valid2)
    {
        GSFrameReg selected1{}, selected2{};
        std::vector<uint8_t> crt1, crt2;
        bool preferred1 = false, preferred2 = false;
        if (copySource(displayFrame1, origin1, width1, height1, false, true, selected1, crt1, preferred1) &&
            copySource(displayFrame2, origin2, width2, height2, false, true, selected2, crt2, preferred2))
        {
            if (logDiag)
            {
                uint32_t crt1RgbNonZero = 0, crt1AlphaNonZero = 0;
                uint32_t crt2RgbNonZero = 0, crt2AlphaNonZero = 0;
                for (size_t i = 0; i < crt1.size(); i += 4) {
                    if (crt1[i] != 0 || crt1[i+1] != 0 || crt1[i+2] != 0) crt1RgbNonZero++;
                    if (crt1[i+3] != 0) crt1AlphaNonZero++;
                }
                for (size_t i = 0; i < crt2.size(); i += 4) {
                    if (crt2[i] != 0 || crt2[i+1] != 0 || crt2[i+2] != 0) crt2RgbNonZero++;
                    if (crt2[i+3] != 0) crt2AlphaNonZero++;
                }
                std::cout << "[gs:diag] crt1 (fbp=" << selected1.fbp << "): rgbNZ=" << crt1RgbNonZero << " aNZ=" << crt1AlphaNonZero
                          << " | crt2 (fbp=" << selected2.fbp << "): rgbNZ=" << crt2RgbNonZero << " aNZ=" << crt2AlphaNonZero
                          << " pmode.alp=" << static_cast<uint32_t>(pmode.alp)
                          << " mmod=" << pmode.mmod << " slbg=" << pmode.slbg << std::endl;
            }
            result.width = std::max(width1, width2);
            result.height = std::max(height1, height2);
            result.pixels.assign(kHostFrameWidth * kHostFrameHeight * 4u, 0u);
            const uint8_t bgR = static_cast<uint8_t>(request.bgcolor);
            const uint8_t bgG = static_cast<uint8_t>(request.bgcolor >> 8u);
            const uint8_t bgB = static_cast<uint8_t>(request.bgcolor >> 16u);
            for (uint32_t y = 0; y < result.height; ++y)
                for (uint32_t x = 0; x < result.width; ++x)
                {
                    uint8_t *dst = result.pixels.data() + (y * kHostFrameWidth + x) * 4u;
                    dst[0] = bgR;
                    dst[1] = bgG;
                    dst[2] = bgB;
                    dst[3] = pmode.alp;
                }
            if (!pmode.slbg)
                for (uint32_t y = 0; y < height2; ++y)
                    std::memcpy(result.pixels.data() + y * kHostFrameWidth * 4u, crt2.data() + y * kHostFrameWidth * 4u, width2 * 4u);
            for (uint32_t y = 0; y < height1; ++y)
                for (uint32_t x = 0; x < width1; ++x)
                {
                    const uint8_t *src = crt1.data() + (y * kHostFrameWidth + x) * 4u;
                    uint8_t *dst = result.pixels.data() + (y * kHostFrameWidth + x) * 4u;
                    const uint32_t factor = pmode.mmod ? pmode.alp : std::min<uint32_t>(255u, static_cast<uint32_t>(src[3]) * 2u);
                    dst[0] = blendPresentationChannel(src[0], dst[0], factor);
                    dst[1] = blendPresentationChannel(src[1], dst[1], factor);
                    dst[2] = blendPresentationChannel(src[2], dst[2], factor);
                    dst[3] = pmode.amod ? dst[3] : src[3];
                }
            normalizePresentationAlpha(result.pixels, result.width, result.height);
            if (fieldMode)
                applyFieldPresentation(result.pixels, result.width, result.height, oddField);
            result.displayFbp = displayFrame1.fbp;
            result.sourceFbp = selected1.fbp;
            return result;
        }
    }

    const GSFrameReg &displayFrame = valid1 ? displayFrame1 : displayFrame2;
    const GSDisplayReadOrigin &origin = valid1 ? origin1 : origin2;
    result.width = valid1 ? width1 : width2;
    result.height = valid1 ? height1 : height2;
    GSFrameReg selected = displayFrame;
    if (!copySource(displayFrame, origin, result.width, result.height, true, false, selected, result.pixels, result.usedPreferred))
        return {};
    if (fieldMode)
        applyFieldPresentation(result.pixels, result.width, result.height, oddField);
    normalizePresentationAlpha(result.pixels, result.width, result.height);
    result.displayFbp = displayFrame.fbp;
    result.sourceFbp = selected.fbp;
    return result;
}
