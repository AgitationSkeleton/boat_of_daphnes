// Gyro aiming: a controller's gyroscope (DualSense, DualShock 4, Switch Pro, Steam Controller ...), read through SDL3's
// gamepad sensor API, steers the aim while the game is in one of its aiming modes - bow, hookshot, grappling hook,
// boomerang, telescope, Picto Box (and, as the player chooses, the first-person look).
//
// How (found by running the game, see BLUEWAKE_TEST_GYRO below): in those modes the main stick is the aim - a stick position
// is a turning rate for Link's facing (which the camera and the arrow follow) and for a pitch ratio the camera keeps
// (dCamera_c's subject work, m388, -1 .. 1). Writing Link's facing directly does not stick (the camera hands its own copy
// back each frame), so the yaw goes in as a push of the main stick shaped by the inverse of the game's own stick curve
// (it then turns at the gyro's rate, up to the stick's full rate of about 108 degrees a second), and the pitch is added to
// the camera's ratio once a game frame, after the game's work (dScnPly_Draw's hook), where it stays. The game's own limits
// (pitch range, ground and wall clamps) still apply. Outside those modes nothing is touched, and the stick keeps working
// alongside.
//
// The design (gravity-aware yaw, bias calibration at rest, sensitivity and inversion per axis) follows what other
// ports do for gyro aiming (2Ship2Harkinian, Shipwright); no code of theirs is used.
#include "runtime/bod.h"
#include "runtime/ctx.h"
#include "runtime/runtime.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_sensor.h>
#include <SDL3/SDL_stdinc.h>
#include <dolphin/pad.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace rt {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kRadToDeg = 180.f / kPi;

// The game's player status words (g_dComIfG_gameInfo.play.mPlayerStatus[0], GZLE01): daPyStts0_* / daPyStts1_*.
constexpr u32 kStatus0 = 0x803C5EA8u + 0x4A28u, kStatus1 = kStatus0 + 4u;
constexpr u32 kFirstPerson = 0x00002000u;                                  // SUBJECT
constexpr u32 kAimItems0 = 0x00001000u | 0x00004000u | 0x00020000u | 0x00080000u | 0x00200000u;  // bow, hookshot, rope, boomerang, telescope
constexpr u32 kPictoBox1 = 0x00000008u;

// The game's objects (GZLE01): the player actor, and dCamera_c inside the camera process (g_dComIfG_gameInfo.play).
constexpr u32 kPlayerPtr = 0x803C5EA8u + 0x48A4u;      // mPlayerInfo[0].mpPlayer
constexpr u32 kCameraPtr = 0x803C5EA8u + 0x4870u;      // mCameraInfo[0].mpCamera (a camera_process_class; its dCamera_c is at 0x244)
constexpr u32 kShapeYaw = 0x20Eu;  // fopAc_ac_c shape_angle.y (s16), logged by the test path
constexpr u32 kBodyPitch = 0x2B4u;                      // daPy_c mBodyAngle.x (s16)
constexpr u32 kSubjTag = 0x378u, kSubjPitch = 0x388u;   // dCamera_c subject work: tag 'SUBJ', pitch ratio (f32)
constexpr u32 kSubjMagic = 0x5355424Au;                 // 'SUBJ'
// How fast the aim turns sideways with the main stick fully pushed, in degrees a second (measured in the running game with a
// full stick in the first-person look: 3.6 degrees a game frame); and the stick value at which the game's aim curve is at full
// rate (JUTGamePad divides a main stick value by 54 after the dead zone; CalcSubjectAngle by 0.7).
constexpr float kYawFullDegPerSec = 108.f;
constexpr float kStickDead = 15.f, kStickSpan = 0.7f * 54.f;  // (measured: the aim's stick position is (stick - 15) / 37.8, full at 1)
// The pitch angle (degrees) the camera's pitch ratio of 1 stands for in the subject styles.
constexpr float kPitchRangeDeg = 70.f;

struct Sample {
    float gyro[3];  // rad/s, controller axes (SDL: x right, y up, z towards the player)
    float accel[3];  // m/s^2
};

std::mutex s_lock;
SDL_Gamepad* s_pad = nullptr;
std::string s_name;
bool s_hasGyro = false, s_hasAccel = false, s_enabledSensors = false;
std::string s_status = "No controller with a gyroscope";

// Calibration: the gyro's resting offset.
float s_bias[3] = {0, 0, 0};
bool s_calibrating = false;
double s_calStart = 0;
double s_calSum[3] = {0, 0, 0};
float s_calMin[3], s_calMax[3];
int s_calCount = 0;
std::atomic<bool> s_recalibrate{false};
bool s_calibratedOnce = false;
double s_calGiveUp = 0;

float s_smooth[2] = {0, 0};      // smoothed yaw / pitch rates (deg/s)
std::atomic<int> s_lastIx{0};
float s_carry = 0;                // the stick's rounding remainder, carried to the next poll
std::atomic<float> s_outPitch{0};  // what the poll hands the game thread (deg/s, up positive)
std::atomic<bool> s_outActive{false};
std::atomic<u64> s_outFrame{0};
double s_lastTime = 0;

double now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// The test path: BLUEWAKE_TEST_GYRO="<pitch>,<yaw>:<frames>@<from frame>;..." holds those turning rates (degrees a second,
// pitch up / yaw left positive, as a controller held upright reports them) for that many presented frames.
struct TestSeg {
    float pitch, yaw;
    u64 from, frames;
};
const std::vector<TestSeg>& testSegments() {
    static const std::vector<TestSeg> v = [] {
        std::vector<TestSeg> out;
        const char* s = std::getenv("BLUEWAKE_TEST_GYRO");
        while (s && *s) {
            char* end = nullptr;
            const float pitch = std::strtof(s, &end);
            if (*end != ',') break;
            s = end + 1;
            const float yaw = std::strtof(s, &end);
            u64 frames = 60, from = 0;
            if (*end == ':') {
                s = end + 1;
                frames = std::strtoull(s, &end, 10);
            }
            if (*end == '@') {
                s = end + 1;
                from = std::strtoull(s, &end, 10);
            }
            out.push_back({pitch, yaw, from, frames});
            if (*end != ';') break;
            s = end + 1;
        }
        return out;
    }();
    return v;
}
bool testActive(u64 frame, float* pitch, float* yaw) {
    for (const TestSeg& t : testSegments())
        if (frame >= t.from && frame < t.from + t.frames) {
            *pitch = t.pitch, *yaw = t.yaw;
            return true;
        }
    return false;
}

void releasePad() {
    if (s_pad && s_enabledSensors) {
        if (s_hasGyro) SDL_SetGamepadSensorEnabled(s_pad, SDL_SENSOR_GYRO, false);
        if (s_hasAccel) SDL_SetGamepadSensorEnabled(s_pad, SDL_SENSOR_ACCEL, false);
    }
    s_pad = nullptr;
    s_hasGyro = s_hasAccel = s_enabledSensors = false;
    s_name.clear();
}

void beginCalibration(double t) {
    s_calibrating = true;
    s_calStart = t;
    s_calCount = 0;
    for (int i = 0; i < 3; i++) s_calSum[i] = 0, s_calMin[i] = 1e9f, s_calMax[i] = -1e9f;
}

// The controller on port 1 (the one Aurora gave player index 0), else the first with a gyroscope.
SDL_Gamepad* findPad() {
    SDL_Gamepad* first = SDL_GetGamepadFromPlayerIndex(0);
    if (first && SDL_GamepadHasSensor(first, SDL_SENSOR_GYRO)) return first;
    SDL_Gamepad* other = nullptr;
    int n = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&n)) {
        for (int i = 0; i < n && !other; i++)
            if (SDL_Gamepad* g = SDL_GetGamepadFromID(ids[i]))
                if (SDL_GamepadHasSensor(g, SDL_SENSOR_GYRO)) other = g;
        SDL_free(ids);
    }
    return other;
}

void acquire(bool wanted, double t) {
    static double s_nextScan = 0;
    SDL_Gamepad* g = s_pad;
    if (t >= s_nextScan) {
        s_nextScan = t + 1.0;  // pads come and go: looked at again each second
        SDL_Gamepad* found = findPad();
        if (found != s_pad) {
            releasePad();
            g = s_pad = found;
            if (g) {
                s_name = SDL_GetGamepadName(g) ? SDL_GetGamepadName(g) : "controller";
                s_hasGyro = SDL_GamepadHasSensor(g, SDL_SENSOR_GYRO);
                s_hasAccel = SDL_GamepadHasSensor(g, SDL_SENSOR_ACCEL);
                log("[gyro] %s: gyro available (%.0f Hz)%s\n", s_name.c_str(), SDL_GetGamepadSensorDataRate(g, SDL_SENSOR_GYRO),
                    s_hasAccel ? ", accelerometer too" : "");
                s_calibratedOnce = false;
            } else {
                log("[gyro] no controller with a gyroscope\n");
            }
        }
    }
    if (g && wanted && !s_enabledSensors) {  // (the sensors are only switched on while the setting is: some pads change report mode)
        if (s_hasGyro) SDL_SetGamepadSensorEnabled(g, SDL_SENSOR_GYRO, true);
        if (s_hasAccel) SDL_SetGamepadSensorEnabled(g, SDL_SENSOR_ACCEL, true);
        s_enabledSensors = true;
        beginCalibration(t);
        s_calGiveUp = t + 4.0;
        log("[gyro] %s: gyro aiming on\n", s_name.c_str());
    } else if (g && !wanted && s_enabledSensors) {
        if (s_hasGyro) SDL_SetGamepadSensorEnabled(g, SDL_SENSOR_GYRO, false);
        if (s_hasAccel) SDL_SetGamepadSensorEnabled(g, SDL_SENSOR_ACCEL, false);
        s_enabledSensors = false;
        s_calibrating = false;
    }
}

// The gyro's turning rates (degrees a second): yaw left positive, pitch up positive.
bool readRates(const Sample& s, double t, float* yaw, float* pitch, bool continuous) {
    // The up direction in the controller's frame is the accelerometer's (it reads +1 g on the axis pointing up at rest):
    // the turn about it is the player's yaw whichever way the controller is held.
    float up[3] = {0, 1, 0};
    const float a2 = s.accel[0] * s.accel[0] + s.accel[1] * s.accel[1] + s.accel[2] * s.accel[2];
    if (a2 > 4.f * 4.f) {
        const float inv = 1.f / std::sqrt(a2);
        up[0] = s.accel[0] * inv, up[1] = s.accel[1] * inv, up[2] = s.accel[2] * inv;
    }
    if (s_calibrating) {
        for (int i = 0; i < 3; i++) {
            s_calSum[i] += s.gyro[i];
            s_calMin[i] = std::min(s_calMin[i], s.gyro[i]);
            s_calMax[i] = std::max(s_calMax[i], s.gyro[i]);
        }
        s_calCount++;
        const double dur = t - s_calStart;
        bool moved = false;
        for (int i = 0; i < 3; i++) moved |= (s_calMax[i] - s_calMin[i]) * kRadToDeg > 4.f;  // more than 4 deg/s of spread: not at rest
        if (moved) {
            beginCalibration(t);  // restart the window: keep still
            s_status = "Calibrating: keep the controller still";
            if (!s_calibratedOnce && t > s_calGiveUp) s_calibrating = false;  // (the start-up calibration does not hold the gyro hostage)
        } else if (dur >= 1.0 && s_calCount >= 8) {
            for (int i = 0; i < 3; i++) s_bias[i] = float(s_calSum[i] / s_calCount);
            s_calibrating = false;
            s_calibratedOnce = true;
            log("[gyro] calibrated: offset %.2f %.2f %.2f deg/s\n", s_bias[0] * kRadToDeg, s_bias[1] * kRadToDeg,
                s_bias[2] * kRadToDeg);
        }
        if (s_calibrating) return false;
    }
    float g[3];
    for (int i = 0; i < 3; i++) g[i] = s.gyro[i] - s_bias[i];
    const float mag = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]) * kRadToDeg;
    if (continuous && mag < 1.2f && s_lastTime > 0) {
        // at rest: the offset follows the drift (about a two-second time constant)
        const float k = float(std::min(1.0, (t - s_lastTime) / 2.0));
        for (int i = 0; i < 3; i++) s_bias[i] += g[i] * k;
    }
    *yaw = (g[0] * up[0] + g[1] * up[1] + g[2] * up[2]) * kRadToDeg;
    *pitch = g[0] * kRadToDeg;
    // (a small dead zone, and light smoothing, so the sensor's noise does not creep the aim)
    for (float* v : {yaw, pitch}) {
        const float m = std::fabs(*v);
        *v = (m < 0.4f ? 0.f : m - 0.4f) * (*v < 0 ? -1.f : 1.f);
    }
    return true;
}

// The stick value (game units) that makes the aim turn at `frac` of its full rate: the inverse of the game's curve
// rationalBezierRatio(x, 1) = (1 - sqrt(1 - x))^2 (the camera's curve weight is 1), from which a stick position x (of 0.7 at
// full) turns the aim.
float stickFor(float frac) {
    const float a = std::min(std::fabs(frac), 1.f);
    if (a < 1e-4f) return 0.f;
    const float v = kStickDead + (2.f * std::sqrt(a) - a) * kStickSpan;  // (the pad's dead zone first: PADClamp's 15)
    return frac < 0 ? -v : v;
}

}  // namespace

// Each host frame (the UI thread): reads the controller and works out the turning rates for the game thread.
void gyroPoll(PADStatus& pad, u64 frame) {
    (void)pad;
    if (frame == ~0ull) {  // (the menu or another window has the input)
        s_outActive = false;
        return;
    }
    const bool enabled = !testSegments().empty() || bod::cvarInt("gSettings.Controls.Gyro.Enabled", 0) != 0;  // (a test run needs no menu)
    const double t = now();
    float testPitch = 0, testYaw = 0;
    const bool testing = testActive(frame, &testPitch, &testYaw);
    static const bool s_rawStick = std::getenv("BLUEWAKE_TEST_GYRO_RAWSTICK") != nullptr;  // (test: the segments' yaw is a main stick value)
    if (s_rawStick && testing) {
        pad.stickX = s8(int(testYaw));
        s_lastIx = int(testYaw);
        return;
    }
    {
        std::lock_guard lk(s_lock);
        acquire(enabled, t);
        if (s_recalibrate.exchange(false) && s_enabledSensors) {
            s_calibratedOnce = true;
            beginCalibration(t);
        }
    }
    if (!enabled) {
        s_smooth[0] = s_smooth[1] = 0;
        s_outActive = false;
        if (!s_pad) s_status = "No controller with a gyroscope";
        else s_status = s_name + ": gyro aiming is off";
        return;
    }

    // In which mode is the game? (only an aiming one takes the gyro; the first-person look when the setting says so)
    static const char* const s_testMode = std::getenv("BLUEWAKE_TEST_GYRO_MODE");
    const int mode = s_testMode ? std::atoi(s_testMode) : bod::cvarInt("gSettings.Controls.Gyro.Mode", 0);
    const u32 s0 = rd32(kStatus0), s1 = rd32(kStatus1);
    const bool aiming = (s0 & kAimItems0) || (s1 & kPictoBox1);
    const bool active = aiming || (mode == 1 && (s0 & kFirstPerson));

    float yawRate = 0, pitchRate = 0;  // deg/s
    bool have = false;
    {
        std::lock_guard lk(s_lock);
        Sample s{};
        if (testing) {
            // a synthetic sample through the same path: a controller held upright, turning as asked
            s.gyro[0] = testPitch / kRadToDeg, s.gyro[1] = testYaw / kRadToDeg;
            s.accel[1] = 9.81f;
            if (s_calibrating) s_calibrating = false;  // (a test gives no time to rest)
            have = readRates(s, t, &yawRate, &pitchRate, false);
        } else if (s_pad && s_enabledSensors && s_hasGyro) {
            bool ok = SDL_GetGamepadSensorData(s_pad, SDL_SENSOR_GYRO, s.gyro, 3);
            if (s_hasAccel) ok = SDL_GetGamepadSensorData(s_pad, SDL_SENSOR_ACCEL, s.accel, 3) && ok;
            else s.accel[1] = 9.81f;
            if (ok) have = readRates(s, t, &yawRate, &pitchRate, bod::cvarInt("gSettings.Controls.Gyro.AutoCalibrate", 1) != 0 && !active);
        }
        if (s_pad && !testing) {
            s_status = s_calibrating ? "Calibrating: keep the controller still"
                       : have       ? s_name + ": gyro ready"
                                    : s_name + ": waiting for the gyro";
        }
        s_lastTime = t;
    }
    s_outActive = have && active;
    if (!have || !active) {
        s_smooth[0] = s_smooth[1] = 0;
        return;
    }

    // sensitivity, inversion
    const float sensX = float(bod::cvarInt("gSettings.Controls.Gyro.SensitivityX", 100)) / 100.f;
    const float sensY = float(bod::cvarInt("gSettings.Controls.Gyro.SensitivityY", 100)) / 100.f;
    yawRate *= sensX * (bod::cvarInt("gSettings.Controls.Gyro.InvertX", 0) ? -1.f : 1.f);
    pitchRate *= sensY * (bod::cvarInt("gSettings.Controls.Gyro.InvertY", 0) ? -1.f : 1.f);
    s_smooth[0] += (yawRate - s_smooth[0]) * 0.7f;
    s_smooth[1] += (pitchRate - s_smooth[1]) * 0.7f;
    s_outPitch = s_smooth[1];
    // yaw: the game turns it at a rate set by the stick (it keeps the result: Link's facing integrates it), so the rate
    // goes in as a main stick push; the pitch is written straight into the camera each frame (gyroFrame)
    const float sx = stickFor(-s_smooth[0] / kYawFullDegPerSec) + s_carry;
    const int ix = int(std::lround(sx));
    s_carry = sx - float(ix);
    pad.stickX = s8(std::clamp(int(pad.stickX) + ix, -127, 127));
    s_lastIx = ix;
    s_outFrame = frame;
}

// The game thread, once a game frame, after the game's work (dScnPly_Draw's hook, bod_api.cpp): the pitch turned since the last
// frame goes into the subject camera's pitch ratio, so the game's own frame sees it whole (a write at an arbitrary retrace can
// land in the middle of the frame's work and be lost).
namespace bod {
void gyroFrame() {
    static double s_last = 0;
    static u64 s_n = 0;
    const double t = now();
    const float dt = s_last > 0 ? float(std::min(t - s_last, 0.1)) : 0.f;
    s_last = t;
    s_n++;
    const float pitch = s_outPitch;  // deg/s, up positive
    const bool active = s_outActive;
    const u32 link = rd32(kPlayerPtr), cam = rd32(kCameraPtr);
    if (!link || !cam) return;
    const u32 dc = cam + 0x244u;
    if (!testSegments().empty() && s_n % 6 == 0)  // (test runs: what the game turns to; Link's shape_angle.y is a 16 bit angle)
        log("[gyro-test] active %d status %08X stick %d rates pitch %.1f | link yaw %.1f deg, body pitch %.1f deg, subject %s ratio %.3f\n", int(active),
            rd32(kStatus0), s_lastIx.load(), pitch, double(s16(rd32(link + kShapeYaw - 2) & 0xFFFFu)) * 360.0 / 65536.0,
            double(s16(rd32(link + kBodyPitch) >> 16)) * 360.0 / 65536.0, rd32(dc + kSubjTag) == kSubjMagic ? "yes" : "no",
            double(flt(rd32(dc + kSubjPitch))));
    if (!active || dt <= 0.f) return;
    // pitch: the subject camera's ratio, while it is the one running
    if (rd32(dc + kSubjTag) == kSubjMagic) {
        float r = flt(rd32(dc + kSubjPitch)) + pitch * dt / kPitchRangeDeg;
        r = std::clamp(r, -1.f, 1.f);
        wr32(dc + kSubjPitch, bits32(r));
    }
}

}  // namespace bod
}  // namespace rt

// For the settings page (src/bod/windows/src/win_settings.cpp).
extern "C" void bluewake_gyro_recalibrate() { rt::s_recalibrate = true; }
extern "C" const char* bluewake_gyro_status() {
    static thread_local std::string s;
    std::lock_guard lk(rt::s_lock);
    s = rt::s_status;
    return s.c_str();
}
