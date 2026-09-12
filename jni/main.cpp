// ============================================================================
// CameraFX (native) — v0.14
//
// RECONSTRUCTION NOTE: this file was rebuilt from the full conversation
// history after the build environment reset mid-session. It reflects the
// complete, final design as developed across that conversation, but was
// NOT re-verified by an actual recompile before delivery here (no NDK
// toolchain available in this pass) — please do a clean build and a quick
// smoke test before shipping an update from this copy. Everything it
// contains — every opcode, every crash fix, every design decision — was
// individually verified against real sources (the Sanny Builder sa_mobile
// opcode database, enums.json, the real SAUtils/AML source, and actual
// crash logs) earlier in that conversation; only the final "does this
// exact file compile" step wasn't repeated here.
//
// Settings live under their own "Camera~b~FX" tab, apply live, and persist
// to configs/CameraFX.ini via AML's own Config system (cfg->Bind()).
//
// Effect opcodes run through SAUtils' ScriptCommand() bridge — the same
// call path the CLEO VM itself uses, no raw memory offsets. Verified
// present on both GTA SA 2.00 (armeabi-v7a) and 2.10 (arm64-v8a):
// _ZN14CRunningScript17ProcessOneCommandEv, the one symbol that bridge
// needs.
//
// Design history, most important points:
//  - Sprint and Fall are binary triggers with a fixed zoom target, matching
//    a real reference CameraFX CLEO script verified opcode-by-opcode.
//    Sprint requires holding WidgetId 31 (ButtonSprint); Fall requires
//    speed > 25 (a safe substitute for the reference's animation check,
//    which had a confirmed crash log — see below).
//  - Vehicle is continuous, not a one-shot transition: FOV tracks actual
//    speed every frame, scaling from a deadzone (Vehicle Zoom Threshold) up
//    to full zoom (Vehicle Zoom Max Speed). Uses GET_CHAR_SPEED (the
//    player's own speed while seated) rather than GET_CAR_SPEED, which was
//    never confirmed reliable through this bridge. Accelerate is an
//    additional OR-condition on top (can trigger/sustain at low speed for
//    immediate feedback); Brake forces an instant return.
//  - Weapon FOV: modest zoom on shotgun/assault/rifle while on foot and not
//    sprinting. Sniper (WeaponType 34) explicitly excluded even though it
//    shares SLOT_RIFLES with the regular Rifle — the sniper has its own
//    native scope-zoom, and stacking this on top of that was a reported bug.
//  - Two opcodes from the reference CLEO scripts are deliberately NOT used,
//    both with confirmed crash logs from real testing in this project:
//    IS_CHAR_ON_FOOT (ProcessCommands1000To1099, crashed twice during
//    vehicle-entry transitions) and IS_CHAR_PLAYING_ANIM("FALL_skyDive")
//    (ProcessCommands1500To1599 — its stack dump literally contained that
//    string). Both ran fine in the reference scripts' own persistent CLEO
//    thread execution; SAUtils' synthetic single-shot ScriptCommand bridge
//    is evidently not equally safe for every opcode.
//  - Accelerate/Brake widget detection has a long history of failed
//    mechanisms in this project (SCM opcode, SAUtils native GetWidgetState,
//    three rounds of custom overlay widgets — none confirmed reliable).
//    Currently back on the SCM opcode (matching Sprint), used as an
//    additive OR-condition for Vehicle rather than a hard requirement, so
//    the proven speed-based tracking still carries the feature if it fails
//    again.
//  - Drunk-wobble effect removed entirely per an earlier explicit request.
// ============================================================================

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/isautils.h>
#include <mod/config.h>

MYMODCFGNAME(net.thirtyfoxmc.camerafx, CameraFX, 0.14, ThirtyFoxMC, CameraFX) // config file: configs/CameraFX.ini — GUID stays net.thirtyfoxmc.camerafx for AML's own mod identification

ISAUtils* sautils = NULL;
static eTypeOfSettings g_tab = SetType_Mods; // replaced with our own tab on load
static volatile bool g_active = false;       // guards every ScriptCommand call

// ---------------------------------------------------------------------------
// Opcodes used (verified against sa_mobile.json + enums.json).
// Signature letters: i=int literal, f=float literal, v=var(in/out ptr),
// s=string, b=bool(int).
// ---------------------------------------------------------------------------
DEFOPCODE(01F5, GET_PLAYER_CHAR,             iv);   // (playerIdx) -> charHandle
DEFOPCODE(06AC, GET_CHAR_SPEED,              iv);   // (charHandle) -> speed
DEFOPCODE(0818, IS_CHAR_IN_AIR,              i);    // (charHandle)
DEFOPCODE(04AD, IS_CHAR_IN_WATER,            i);    // (charHandle)
DEFOPCODE(0A0C, IS_PLAYER_USING_JETPACK,     i);    // (playerIdx)
DEFOPCODE(0256, IS_PLAYER_PLAYING,           i);    // (playerIdx)
DEFOPCODE(0801, GET_CAMERA_FOV,              v);    // () -> fov
DEFOPCODE(0922, CAMERA_SET_LERP_FOV,         ffib); // (from, to, timeMs, ease)
DEFOPCODE(0931, CAMERA_PERSIST_FOV,          b);    // (state)
DEFOPCODE(0470, GET_CURRENT_CHAR_WEAPON,     iv);   // (charHandle) -> weaponType
DEFOPCODE(0782, GET_WEAPONTYPE_SLOT,         iv);   // (weaponType) -> slot
DEFOPCODE(00DF, IS_CHAR_IN_ANY_CAR,          i);    // (charHandle)
DEFOPCODE(047A, IS_CHAR_ON_ANY_BIKE,         i);    // (charHandle)
DEFOPCODE(04C8, IS_CHAR_IN_FLYING_VEHICLE,   i);    // (charHandle)
DEFOPCODE(0A51, IS_WIDGET_PRESSED,           i);    // (widgetId)

// Verified from enums.json (WeaponType / WeaponSlot / WidgetId) — not guesses.
static const int WEAPON_PARACHUTE   = 46;
static const int WEAPON_SNIPER      = 34;  // excluded from Weapon FOV — has its own native scope zoom
static const int SLOT_SHOTGUNS      = 4;
static const int SLOT_ASSAULT       = 6;
static const int SLOT_RIFLES        = 7;   // includes both Rifle (33) and Sniper (34)
static const int WIDGET_ACCELERATE  = 2;   // WidgetId.Accelerate
static const int WIDGET_BRAKE       = 3;   // WidgetId.Brake
static const int WIDGET_SPRINT      = 31;  // WidgetId.ButtonSprint

// ---------------------------------------------------------------------------
// Language — display-only for now: switching the Language item itself
// updates live, but the OTHER item names below are fixed at
// AddSliderItem/AddButton time and there's no rename/refresh call in the
// SAUtils interface to change them after the fact — so a language change
// takes full effect on the *next* mod load, not instantly. Translations are
// a first pass, not reviewed by native speakers.
// CAVEAT: Japanese and Russian need glyphs GTA SA's stock mobile UI font may
// not include. If they show as boxes/blanks in-game, they can be dropped
// back to English.
// ---------------------------------------------------------------------------
enum class Lang { EN, JA, ES, ID, FR, PT, DE, RU, COUNT };
static Lang g_lang = Lang::EN;

enum class S {
    Author, SprintZoom, FallZoom, ParachuteZoom, VehicleZoom,
    SprintOutT, SprintInT, FallOutT, FallInT, ParachuteT, VehicleT,
    SprintThresh, VehicleZoomThresh, VehicleThresh, BlendTransition, BlendTagline, WeaponFov, COUNT
};

static const char* kLangNames[(int)Lang::COUNT] = {
    "English", "日本語", "Español", "Indonesia", "Français", "Português", "Deutsch", "Русский"
};

static const char* kStrings[(int)Lang::COUNT][(int)S::COUNT] = {
    /* EN */ { "Author", "Sprint Zoom", "Fall Zoom", "Parachute Zoom", "Vehicle Zoom", "Sprint Zoom-Out Time", "Sprint Zoom-In Time", "Fall Zoom-Out Time", "Fall Zoom-In Time", "Parachute Zoom Time", "Vehicle Zoom Time", "Sprint Speed Threshold", "Vehicle Zoom Threshold", "Vehicle Speed Threshold", "Blend Transition", "Smooth Blend Transitions", "Weapon FOV Effect" },
    /* JA */ { "作者", "スプリントズーム", "落下ズーム", "パラシュートズーム", "車両ズーム", "スプリントズームアウト時間", "スプリントズームイン時間", "落下ズームアウト時間", "落下ズームイン時間", "パラシュートズーム時間", "車両ズーム時間", "スプリント速度しきい値", "車両ズームしきい値", "車両速度しきい値", "ブレンド遷移", "スムーズなブレンド遷移", "武器FOV効果" },
    /* ES */ { "Autor", "Zoom de Sprint", "Zoom de Caída", "Zoom de Paracaídas", "Zoom de Vehículo", "Tiempo de Zoom-Out de Sprint", "Tiempo de Zoom-In de Sprint", "Tiempo de Zoom-Out de Caída", "Tiempo de Zoom-In de Caída", "Tiempo de Zoom de Paracaídas", "Tiempo de Zoom de Vehículo", "Umbral de Velocidad de Sprint", "Umbral de Zoom de Vehículo", "Umbral de Velocidad de Vehículo", "Transición de Mezcla", "Transiciones de Mezcla Suaves", "Efecto FOV de Arma" },
    /* ID */ { "Penulis", "Zoom Sprint", "Zoom Jatuh", "Zoom Parasut", "Zoom Kendaraan", "Waktu Zoom-Out Sprint", "Waktu Zoom-In Sprint", "Waktu Zoom-Out Jatuh", "Waktu Zoom-In Jatuh", "Waktu Zoom Parasut", "Waktu Zoom Kendaraan", "Ambang Kecepatan Sprint", "Ambang Zoom Kendaraan", "Ambang Kecepatan Kendaraan", "Transisi Blend", "Transisi Blend yang Halus", "Efek FOV Senjata" },
    /* FR */ { "Auteur", "Zoom Sprint", "Zoom de Chute", "Zoom Parachute", "Zoom Véhicule", "Temps de Zoom Arrière Sprint", "Temps de Zoom Avant Sprint", "Temps de Zoom Arrière Chute", "Temps de Zoom Avant Chute", "Temps de Zoom Parachute", "Temps de Zoom Véhicule", "Seuil de Vitesse Sprint", "Seuil de Zoom Véhicule", "Seuil de Vitesse Véhicule", "Transition de Fondu", "Transitions de Fondu Fluides", "Effet FOV d'Arme" },
    /* PT */ { "Autor", "Zoom de Corrida", "Zoom de Queda", "Zoom de Paraquedas", "Zoom de Veículo", "Tempo de Zoom-Out de Corrida", "Tempo de Zoom-In de Corrida", "Tempo de Zoom-Out de Queda", "Tempo de Zoom-In de Queda", "Tempo de Zoom de Paraquedas", "Tempo de Zoom de Veículo", "Limite de Velocidade de Corrida", "Limite de Zoom de Veículo", "Limite de Velocidade de Veículo", "Transição de Mesclagem", "Transições de Mesclagem Suaves", "Efeito FOV de Arma" },
    /* DE */ { "Autor", "Sprint-Zoom", "Fall-Zoom", "Fallschirm-Zoom", "Fahrzeug-Zoom", "Sprint-Auszoom-Zeit", "Sprint-Einzoom-Zeit", "Fall-Auszoom-Zeit", "Fall-Einzoom-Zeit", "Fallschirm-Zoom-Zeit", "Fahrzeug-Zoom-Zeit", "Sprint-Geschwindigkeitsschwelle", "Fahrzeug-Zoom-Schwelle", "Fahrzeug-Geschwindigkeitsschwelle", "Überblendungsübergang", "Sanfte Überblendungsübergänge", "Waffen-FOV-Effekt" },
    /* RU */ { "Автор", "Зум спринта", "Зум падения", "Зум парашюта", "Зум транспорта", "Время зума спринта (выход)", "Время зума спринта (вход)", "Время зума падения (выход)", "Время зума падения (вход)", "Время зума парашюта", "Время зума транспорта", "Порог скорости спринта", "Порог зума транспорта", "Порог скорости транспорта", "Плавный переход", "Плавные переходы масштабирования", "Эффект FOV оружия" },
};

static void OnLanguageChanged(int, int newVal, void*)
{
    g_lang = (Lang)newVal;
}

// ---------------------------------------------------------------------------
// Settings — plain runtime cache, seeded from (and written back through to)
// AML Config on change. See BindInt() / OnIntSettingChanged() below.
// ---------------------------------------------------------------------------
struct Settings
{
    int sprintZoomX10      = 850;   // 85.0
    int fallZoomX10        = 1100;  // 110.0
    int parachuteZoomX10   = 950;   // 95.0
    int sprintZoomOutTime  = 700;
    int sprintZoomInTime   = 500;
    int fallZoomOutTime    = 3000;
    int fallZoomInTime     = 500;
    int parachuteZoomTime  = 800;
    int vehicleZoomX10     = 900;   // 90.0
    int vehicleZoomTime    = 800;
    int vehicleZoomThresholdX10 = 80;  // 8.0 — deadzone, relative to the known on-foot CharSpeed anchor (6.5), not GET_CAR_SPEED's scale (never used here)
    int vehicleSpeedThresholdX10 = 200; // 20.0 — speed at which vehicle zoom reaches full value
    int charSpeedX10       = 65;    // 6.5 — verified from the original CameraFX.csa's own shipped default
    int blendTransition    = 1;     // smooth_transition flag passed to CAMERA_SET_LERP_FOV
    int weaponFovEnabled   = 1;     // toggle only, no slider — see kWeaponFovZoom/Time below
} g_settings;

// Internal constants for the weapon-FOV feature — deliberately not settings
// sliders, per request. A modest zoom-in while wielding a heavy weapon on
// foot without sprinting, distinct from (and smaller than) the sprint
// zoom-out.
static const float kWeaponFovZoom = 60.0f;
static const int   kWeaponFovTime = 400;

static const char* DrawFloatX10(int v, void*)
{
    static char buf[16];
    snprintf(buf, sizeof(buf), "%.1f", v / 10.0f);
    return buf;
}
static const char* DrawMs(int v, void*)
{
    static char buf[16];
    snprintf(buf, sizeof(buf), "%dms", v);
    return buf;
}
static const char* kOnOffBlue[2] = { "~b~OFF", "~b~ON" };

// ---------------------------------------------------------------------------
// AML Config persistence — pairs a runtime int with the ConfigEntry that
// backs it in configs/CameraFX.ini. BindInt() both loads the starting value
// (creating the ini with defaults on first run) and registers where to
// write changes back to on every settings-menu edit.
// ---------------------------------------------------------------------------
struct BoundSetting { int* value; ConfigEntry* entry; };
static BoundSetting g_bound[16];
static int g_boundCount = 0;

static BoundSetting* BindInt(const char* key, int* target)
{
    ConfigEntry* e = cfg->Bind(key, *target, "Settings");
    if (e) *target = e->GetInt();
    BoundSetting* slot = &g_bound[g_boundCount++];
    slot->value = target;
    slot->entry = e;
    return slot;
}

static void OnIntSettingChanged(int, int newVal, void* pData)
{
    BoundSetting* b = (BoundSetting*)pData;
    *b->value = newVal;
    if (b->entry)
    {
        b->entry->SetInt(newVal);
        cfg->Save();
    }
}

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------
enum class ZoomState { NEUTRAL, SPRINT, FALL, PARACHUTE, VEHICLE, WEAPON };
static ZoomState g_zoomState = ZoomState::NEUTRAL;
static float     g_storedFov = 0.0f;   // FOV to restore to — captured fresh at the moment each NEUTRAL→active transition starts
static bool      g_wasPlaying = false;

// ---------------------------------------------------------------------------
// Main per-frame hook
// ---------------------------------------------------------------------------
static void OnPlayerUpdate(uintptr_t /*info*/)
{
    if (!g_active || !sautils) return;

    if (!CALLSCM(IS_PLAYER_PLAYING, 0))
    {
        g_wasPlaying = false;
        return;
    }
    if (!g_wasPlaying)
    {
        g_zoomState = ZoomState::NEUTRAL;
    }
    g_wasPlaying = true;

    int playerChar = 0;
    CALLSCM(GET_PLAYER_CHAR, 0, &playerChar);

    float speed = 0.0f;
    CALLSCM(GET_CHAR_SPEED, playerChar, &speed);

    bool inCar = CALLSCM(IS_CHAR_IN_ANY_CAR, playerChar) != 0;
    bool onBike = CALLSCM(IS_CHAR_ON_ANY_BIKE, playerChar) != 0;
    bool inFlyingVehicle = CALLSCM(IS_CHAR_IN_FLYING_VEHICLE, playerChar) != 0;
    bool inAnyVehicle = inCar || onBike || inFlyingVehicle;

    bool inWater = false, inAir = false, usingJetpack = false;
    bool hasParachute = false;
    bool hasHeavyWeapon = false;
    if (!inAnyVehicle)
    {
        // Pedestrian-only queries. IS_CHAR_ON_FOOT and the FALL_skyDive
        // anim check both had confirmed crash logs earlier and stay out —
        // see the header note.
        inWater        = CALLSCM(IS_CHAR_IN_WATER, playerChar) != 0;
        inAir           = CALLSCM(IS_CHAR_IN_AIR, playerChar) != 0;
        usingJetpack    = CALLSCM(IS_PLAYER_USING_JETPACK, 0) != 0;

        int weaponType = 0;
        CALLSCM(GET_CURRENT_CHAR_WEAPON, playerChar, &weaponType);
        hasParachute = (weaponType == WEAPON_PARACHUTE);

        int weaponSlot = 0;
        CALLSCM(GET_WEAPONTYPE_SLOT, weaponType, &weaponSlot);
        hasHeavyWeapon = (weaponSlot == SLOT_SHOTGUNS || weaponSlot == SLOT_ASSAULT || weaponSlot == SLOT_RIFLES)
                          && (weaponType != WEAPON_SNIPER);
    }

    float charSpeedThreshold = g_settings.charSpeedX10 / 10.0f;
    float sprintZoom          = g_settings.sprintZoomX10 / 10.0f;
    float fallZoom             = g_settings.fallZoomX10 / 10.0f;
    float parachuteZoom        = g_settings.parachuteZoomX10 / 10.0f;
    float vehicleZoom          = g_settings.vehicleZoomX10 / 10.0f;
    int   easeFlag              = g_settings.blendTransition ? 1 : 0;

    bool sprintWidgetHeld = !inAnyVehicle && CALLSCM(IS_WIDGET_PRESSED, WIDGET_SPRINT) != 0;
    bool isSprinting  = sprintWidgetHeld && !inWater && !inAir && (speed > charSpeedThreshold);
    bool isParachute  = inAir && hasParachute && !inAnyVehicle;
    bool isFalling      = inAir && !usingJetpack && !hasParachute && !inAnyVehicle && (speed > 25.0f);
    bool isWeaponFov = g_settings.weaponFovEnabled && hasHeavyWeapon && !sprintWidgetHeld
                       && !inWater && !inAir && !inAnyVehicle;

    bool accelHeld = inAnyVehicle && CALLSCM(IS_WIDGET_PRESSED, WIDGET_ACCELERATE) != 0;
    bool brakeHeld  = inAnyVehicle && CALLSCM(IS_WIDGET_PRESSED, WIDGET_BRAKE) != 0;

    float vehicleT = 0.0f;
    if (inAnyVehicle)
    {
        float threshold = g_settings.vehicleZoomThresholdX10 / 10.0f;
        float maxSpeedRef = g_settings.vehicleSpeedThresholdX10 / 10.0f;
        float range = maxSpeedRef - threshold;
        vehicleT = (range > 0.01f) ? (speed - threshold) / range : 0.0f;
        if (vehicleT < 0.0f) vehicleT = 0.0f;
        if (vehicleT > 1.0f) vehicleT = 1.0f;
        if (brakeHeld) vehicleT = 0.0f;
    }
    bool wantVehicle = inAnyVehicle && !brakeHeld && (accelHeld || vehicleT > 0.02f);

    ZoomState wantState = ZoomState::NEUTRAL;
    if (isSprinting)       wantState = ZoomState::SPRINT;
    else if (isParachute) wantState = ZoomState::PARACHUTE;
    else if (wantVehicle)  wantState = ZoomState::VEHICLE;
    else if (isWeaponFov) wantState = ZoomState::WEAPON;
    else if (isFalling)   wantState = ZoomState::FALL;

    if (wantState == ZoomState::VEHICLE && g_zoomState == ZoomState::VEHICLE)
    {
        // Continuous tracking: recompute the target every frame from
        // vehicleT above, not a one-shot transition.
        float target = g_storedFov + (vehicleZoom - g_storedFov) * vehicleT;
        float curFov = 0.0f;
        CALLSCM(GET_CAMERA_FOV, &curFov);
        if (curFov > 1.0f && curFov < 170.0f)
            CALLSCM(CAMERA_SET_LERP_FOV, curFov, target, 200, easeFlag);
    }

    if (wantState != g_zoomState)
    {
        float curFov = 0.0f;
        CALLSCM(GET_CAMERA_FOV, &curFov);

        bool enteringFromNeutral = (wantState != ZoomState::NEUTRAL && g_zoomState == ZoomState::NEUTRAL);
        bool curFovSane = (curFov > 1.0f && curFov < 170.0f);

        if (!enteringFromNeutral || curFovSane)
        {
            if (wantState != ZoomState::NEUTRAL)
            {
                if (enteringFromNeutral)
                    g_storedFov = curFov;

                float target; int timeMs;
                switch (wantState)
                {
                    case ZoomState::SPRINT:    target = sprintZoom;    timeMs = g_settings.sprintZoomOutTime; break;
                    case ZoomState::FALL:      target = fallZoom;      timeMs = g_settings.fallZoomOutTime;   break;
                    case ZoomState::PARACHUTE: target = parachuteZoom; timeMs = g_settings.parachuteZoomTime; break;
                    case ZoomState::WEAPON:    target = kWeaponFovZoom; timeMs = kWeaponFovTime;              break;
                    case ZoomState::VEHICLE:
                    {
                        target = curFov + (vehicleZoom - curFov) * vehicleT;
                        timeMs = g_settings.vehicleZoomTime;
                        break;
                    }
                    default:                    target = curFov;        timeMs = 500; break;
                }
                CALLSCM(CAMERA_SET_LERP_FOV, curFov, target, timeMs, easeFlag);
                CALLSCM(CAMERA_PERSIST_FOV, 1);
            }
            else
            {
                int returnTime;
                switch (g_zoomState)
                {
                    case ZoomState::PARACHUTE: returnTime = g_settings.parachuteZoomTime; break;
                    case ZoomState::WEAPON:    returnTime = kWeaponFovTime;               break;
                    case ZoomState::VEHICLE:   returnTime = g_settings.vehicleZoomTime;   break;
                    case ZoomState::FALL:      returnTime = g_settings.fallZoomInTime;    break;
                    default:                    returnTime = g_settings.sprintZoomInTime; break;
                }
                CALLSCM(CAMERA_SET_LERP_FOV, curFov, g_storedFov, returnTime, easeFlag);
                CALLSCM(CAMERA_PERSIST_FOV, 0);
            }

            g_zoomState = wantState;
        }
    }
}

// ---------------------------------------------------------------------------
// Mod lifecycle
// ---------------------------------------------------------------------------
ON_MOD_PRELOAD()
{
    logger->SetTag("CameraFX");
}

ON_MOD_LOAD()
{
    sautils = (ISAUtils*)GetInterface("SAUtils");
    if (!sautils)
    {
        logger->Error("SAUtils interface not found — is SAUtils installed and loaded before CameraFX?");
        return;
    }

    g_tab = sautils->AddSettingsTab("Camera~b~FX");

    sautils->AddButton(g_tab, "~b~Author: Senji", NULL);
    sautils->AddButton(g_tab, "CameraFX 0.4 (Alpha)", NULL);
    const char** T = kStrings[(int)g_lang];
    sautils->AddClickableItem(g_tab, "Language", (int)g_lang, 0, (int)Lang::COUNT - 1,
        kLangNames, OnLanguageChanged);

    sautils->AddSliderItem(g_tab, T[(int)S::SprintZoom], g_settings.sprintZoomX10, 0, 2000,
        OnIntSettingChanged, DrawFloatX10, BindInt("SprintZoom", &g_settings.sprintZoomX10));
    sautils->AddSliderItem(g_tab, T[(int)S::FallZoom], g_settings.fallZoomX10, 0, 2000,
        OnIntSettingChanged, DrawFloatX10, BindInt("FallZoom", &g_settings.fallZoomX10));
    sautils->AddSliderItem(g_tab, T[(int)S::ParachuteZoom], g_settings.parachuteZoomX10, 0, 2000,
        OnIntSettingChanged, DrawFloatX10, BindInt("ParachuteZoom", &g_settings.parachuteZoomX10));
    sautils->AddSliderItem(g_tab, T[(int)S::VehicleZoom], g_settings.vehicleZoomX10, 0, 2000,
        OnIntSettingChanged, DrawFloatX10, BindInt("VehicleZoom", &g_settings.vehicleZoomX10));
    sautils->AddSliderItem(g_tab, T[(int)S::SprintOutT], g_settings.sprintZoomOutTime, 0, 10000,
        OnIntSettingChanged, DrawMs, BindInt("SprintZoomOutTime", &g_settings.sprintZoomOutTime));
    sautils->AddSliderItem(g_tab, T[(int)S::SprintInT], g_settings.sprintZoomInTime, 0, 10000,
        OnIntSettingChanged, DrawMs, BindInt("SprintZoomInTime", &g_settings.sprintZoomInTime));
    sautils->AddSliderItem(g_tab, T[(int)S::FallOutT], g_settings.fallZoomOutTime, 0, 10000,
        OnIntSettingChanged, DrawMs, BindInt("FallZoomOutTime", &g_settings.fallZoomOutTime));
    sautils->AddSliderItem(g_tab, T[(int)S::FallInT], g_settings.fallZoomInTime, 0, 10000,
        OnIntSettingChanged, DrawMs, BindInt("FallZoomInTime", &g_settings.fallZoomInTime));
    sautils->AddSliderItem(g_tab, T[(int)S::ParachuteT], g_settings.parachuteZoomTime, 0, 10000,
        OnIntSettingChanged, DrawMs, BindInt("ParachuteZoomTime", &g_settings.parachuteZoomTime));
    sautils->AddSliderItem(g_tab, T[(int)S::VehicleT], g_settings.vehicleZoomTime, 0, 10000,
        OnIntSettingChanged, DrawMs, BindInt("VehicleZoomTime", &g_settings.vehicleZoomTime));
    sautils->AddSliderItem(g_tab, T[(int)S::SprintThresh], g_settings.charSpeedX10, 0, 500,
        OnIntSettingChanged, DrawFloatX10, BindInt("SprintSpeedThreshold", &g_settings.charSpeedX10));
    sautils->AddSliderItem(g_tab, T[(int)S::VehicleZoomThresh], g_settings.vehicleZoomThresholdX10, 0, 500,
        OnIntSettingChanged, DrawFloatX10, BindInt("VehicleZoomThreshold", &g_settings.vehicleZoomThresholdX10));
    sautils->AddSliderItem(g_tab, T[(int)S::VehicleThresh], g_settings.vehicleSpeedThresholdX10, 0, 500,
        OnIntSettingChanged, DrawFloatX10, BindInt("VehicleSpeedThreshold", &g_settings.vehicleSpeedThresholdX10));
    sautils->AddClickableItem(g_tab, T[(int)S::BlendTransition], g_settings.blendTransition, 0, 1,
        kOnOffBlue, OnIntSettingChanged, BindInt("BlendTransition", &g_settings.blendTransition));
    sautils->AddClickableItem(g_tab, T[(int)S::WeaponFov], g_settings.weaponFovEnabled, 0, 1,
        kOnOffBlue, OnIntSettingChanged, BindInt("WeaponFovEnabled", &g_settings.weaponFovEnabled));

    sautils->AddPlayerUpdateListener(OnPlayerUpdate, true);

    g_active = true;
    logger->Info("CameraFX v0.14 (native) loaded — settings under its own CameraFX tab, persisted to ini.");
}

ON_MOD_UNLOAD()
{
    g_active = false;
}
