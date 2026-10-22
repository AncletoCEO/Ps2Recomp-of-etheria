#include "runtime/ps2_pad.h"
#include "ps2_host_backend.h"
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>

namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;
    constexpr uint8_t kPadStickCenter = 0x80;

    constexpr uint16_t PAD_LEFT = 0x0080u;
    constexpr uint16_t PAD_DOWN = 0x0040u;
    constexpr uint16_t PAD_RIGHT = 0x0020u;
    constexpr uint16_t PAD_UP = 0x0010u;
    constexpr uint16_t PAD_START = 0x0008u;
    constexpr uint16_t PAD_R3 = 0x0004u;
    constexpr uint16_t PAD_L3 = 0x0002u;
    constexpr uint16_t PAD_SELECT = 0x0001u;
    constexpr uint16_t PAD_SQUARE = 0x8000u;
    constexpr uint16_t PAD_CROSS = 0x4000u;
    constexpr uint16_t PAD_CIRCLE = 0x2000u;
    constexpr uint16_t PAD_TRIANGLE = 0x1000u;
    constexpr uint16_t PAD_R1 = 0x0800u;
    constexpr uint16_t PAD_L1 = 0x0400u;
    constexpr uint16_t PAD_R2 = 0x0200u;
    constexpr uint16_t PAD_L2 = 0x0100u;

    void applySyntheticButton(uint16_t &btns, const char *name)
    {
        if (std::strcmp(name, "start") == 0) btns &= ~PAD_START;
        else if (std::strcmp(name, "cross") == 0) btns &= ~PAD_CROSS;
        else if (std::strcmp(name, "circle") == 0) btns &= ~PAD_CIRCLE;
        else if (std::strcmp(name, "square") == 0) btns &= ~PAD_SQUARE;
        else if (std::strcmp(name, "triangle") == 0) btns &= ~PAD_TRIANGLE;
        else if (std::strcmp(name, "select") == 0) btns &= ~PAD_SELECT;
        else if (std::strcmp(name, "up") == 0) btns &= ~PAD_UP;
        else if (std::strcmp(name, "down") == 0) btns &= ~PAD_DOWN;
        else if (std::strcmp(name, "left") == 0) btns &= ~PAD_LEFT;
        else if (std::strcmp(name, "right") == 0) btns &= ~PAD_RIGHT;
    }

    uint16_t g_syntheticPressedMask = 0u;
}

// Harness determinista (SWORD_AUTOPRESS): fuerzan botones desde el hook de frame sin pasar
// por relojes. Convencion PS2: bit a 1 = pulsado.
extern "C" void PS2SetSyntheticPadMask(uint16_t mask)
{
    g_syntheticPressedMask = mask;
}

bool PSPadBackend::readState(int /*port*/, int /*slot*/, uint8_t *data, size_t size)
{
    if (!data || size < 32)
        return false;

    std::memset(data, 0, 32);
    data[0] = 0x01;
    data[1] = kPadAnalogMarker;
    data[2] = 0xFF;
    data[3] = 0xFF;
    data[4] = data[5] = data[6] = data[7] = kPadStickCenter;

    uint16_t btns = 0xFFFFu;
    constexpr int kGamepad = 0;
    const bool useGamepad = IsGamepadAvailable(kGamepad);
    auto clearBit = [&btns](uint16_t mask)
    { btns &= ~mask; };

    if (useGamepad)
    {
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_UP))
            clearBit(PAD_UP);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_DOWN))
            clearBit(PAD_DOWN);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_LEFT))
            clearBit(PAD_LEFT);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_RIGHT))
            clearBit(PAD_RIGHT);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN))
            clearBit(PAD_CROSS);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT))
            clearBit(PAD_CIRCLE);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_LEFT))
            clearBit(PAD_SQUARE);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_UP))
            clearBit(PAD_TRIANGLE);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_1))
            clearBit(PAD_L1);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_1))
            clearBit(PAD_R1);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_2))
            clearBit(PAD_L2);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_2))
            clearBit(PAD_R2);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_MIDDLE_RIGHT))
            clearBit(PAD_START);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_MIDDLE_LEFT))
            clearBit(PAD_SELECT);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_THUMB))
            clearBit(PAD_L3);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_THUMB))
            clearBit(PAD_R3);

        float lx = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_LEFT_X);
        float ly = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_LEFT_Y);
        float rx = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_RIGHT_X);
        float ry = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_RIGHT_Y);
        data[6] = static_cast<uint8_t>(128 + lx * 127);
        data[7] = static_cast<uint8_t>(128 + ly * 127);
        data[4] = static_cast<uint8_t>(128 + rx * 127);
        data[5] = static_cast<uint8_t>(128 + ry * 127);
    }
    else
    {
        if (IsKeyDown(KEY_UP) || IsKeyDown(KEY_W))
            clearBit(PAD_UP);
        if (IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_S))
            clearBit(PAD_DOWN);
        if (IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_A))
            clearBit(PAD_LEFT);
        if (IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D))
            clearBit(PAD_RIGHT);
        if (IsKeyDown(KEY_X) || IsKeyDown(KEY_SPACE))
            clearBit(PAD_CROSS);
        if (IsKeyDown(KEY_C) || IsKeyDown(KEY_ESCAPE))
            clearBit(PAD_CIRCLE);
        if (IsKeyDown(KEY_Z) || IsKeyDown(KEY_KP_0))
            clearBit(PAD_SQUARE);
        if (IsKeyDown(KEY_V) || IsKeyDown(KEY_KP_1))
            clearBit(PAD_TRIANGLE);
        if (IsKeyDown(KEY_Q))
            clearBit(PAD_L1);
        if (IsKeyDown(KEY_E))
            clearBit(PAD_R1);
        if (IsKeyDown(KEY_LEFT_SHIFT))
            clearBit(PAD_L2);
        if (IsKeyDown(KEY_RIGHT_SHIFT))
            clearBit(PAD_R2);
        if (IsKeyDown(KEY_ENTER))
            clearBit(PAD_START);
        if (IsKeyDown(KEY_TAB))
            clearBit(PAD_SELECT);
    }

    // SWORD_PAD_PRESS=<start|cross|circle|square|triangle|select|up|down|left|right>
    // pulsa un botón de forma permanente (harness headless: sin esto el pad queda neutro).
    // SWORD_PAD_TOGGLE=1 alterna 1 s on / 1 s off para generar flancos (no sólo el inicial).
    if (const char *pe = std::getenv("SWORD_PAD_PRESS");
        pe != nullptr && *pe != '\0' && std::getenv("SWORD_PAD_TIMES") == nullptr)
    {
        bool on = true;
        if (std::getenv("SWORD_PAD_TOGGLE") != nullptr)
            on = std::fmod(GetTime(), 2.0) < 1.0;
        // 2026-10-20: pulsacion unica para el selector/language (press 250 ms a los 5 s) y
        // despues esperar sin volver a pulsar. SWORD_PAD_ONCE=1.
        if (const char *once = std::getenv("SWORD_PAD_ONCE"); once != nullptr && *once != '\0')
        {
            const double t = GetTime();
            on = (t >= 5.0 && t < 5.25);
        }
        if (on)
        {
            applySyntheticButton(btns, pe);
        }
    }

    // SWORD_PAD_TIMES=<seg,seg,...>: un pulso de 250 ms en cada instante indicado (segundos),
    // para avanzar fases encadenadas (selector -> language -> FMV) sin dejar el boton sostenido.
    if (const char *te = std::getenv("SWORD_PAD_TIMES"); te != nullptr && *te != '\0')
    {
        static const std::vector<double> s_times = [te]()
        {
            std::vector<double> times;
            const char *cursor = te;
            while (*cursor != '\0')
            {
                char *end = nullptr;
                const double value = std::strtod(cursor, &end);
                if (end == cursor)
                    break;
                times.push_back(value);
                cursor = end;
                if (*cursor == ',')
                    ++cursor;
            }
            return times;
        }();
        const double now = GetTime();
        for (const double start : s_times)
        {
            if (now >= start && now < start + 0.25)
            {
                const char *button = std::getenv("SWORD_PAD_PRESS");
                if (button != nullptr && *button != '\0')
                    applySyntheticButton(btns, button);
                break;
            }
        }
    }

    btns &= static_cast<uint16_t>(~g_syntheticPressedMask);

    data[2] = static_cast<uint8_t>(btns & 0xFF);
    data[3] = static_cast<uint8_t>(btns >> 8);
    return true;
}
