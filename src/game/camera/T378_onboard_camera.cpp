/* T378 - OnBoardCamera (0x001FF788..0x00201418).
 *
 * The cameras on the followed car.  A car file lists its camera mounts
 * (CarModelInfo +0x68 / +0x6C, 0x40 bytes each: a point, three rotations in
 * degrees, a zoom and flags).  Mounts 0 and 1 are the two fixed views: they
 * turn with the car (mount 1 leaves the car's lag yaw and pitch out:
 * car_lag_yaw / car_lag_pitch), and mount 0 also sways with the wheels at
 * speed.  The other mounts (the cockpit-like ones) take the mount's own
 * rotation and a random head shake that grows with the speed.  The buttons
 * of the RaceInput step through the mounts with MOUNT_SELECTABLE; holding
 * the look-back button turns the view round (yaw + pi).
 *
 * getChangeLevel reports isViewBlocked: the mount's point is off the
 * runway or inside another car's collision box; isInsideCar does the box
 * test for one car.
 *
 * Retail T378's .rodata (0x0033E030..0x0033E3C0): "bad_alloc" (nothing
 * references it: DATA_START), the mount-name table's long names and the
 * table (unreferenced), the two box-normal tables, the vtable, the type
 * names OnBoardCamera, RaceCameraBase, UnitArena<64> (reverse definition
 * order: T153, then RaceCameraBase's header, then the class), then the file
 * static g_0033E370 (deferred to the end).  Its .sdata (0x003529C8..
 * 0x00352A10, unreferenced: DATA_START) is the table's short names and the
 * word 20.  Its .data: g_002DB7B0, then the frame info.
 */
#include <new>                              /* (unused: "bad_alloc") */
#define GAME_T153_EMITS_TYPEINFO            /* UnitArena<64>'s type name */
#include "game/race/core/T153_race_course.h"          /* (seen: UnitArena<64>) */
#include "game/camera/T383_race_camera_base.h"     /* (seen: RaceCameraBase, defined before OnBoardCamera) */
#include "game/camera/T378_onboard_camera.h"
#include "game/camera/onboard_camera_internal.h"
#include "game/enhanced.h"                  /* ENH_CAMERA_OCCLUSION_BOX */
#include "game/app/pc_freecam.h"            /* PC: the chase camera's free orbit (update) */
#include "game/app/pc_detail.h"             /* PC: pc_rear_view_bumper / _chase (update) */
#include "game/app/pc_debugcam.h"           /* PC: pc_debugcam_mount (update) */
#include "game/app/pc_headcam.h"            /* PC: the cockpit camera (applyMountView, update) */
#include "game/app/pc_customcam.h"          /* PC: the user's own cameras (applyMountView, update, pcNextView) */
#include "game/app/pc_vr.h"                 /* PC VR: the headset's shake (applyMountView, update) */
#include "game/save/T426_controller_config.h"   /* PC: ControllerConfig::keyAxis (update) */
#include <string.h>                         /* PC: memset (pc_freecam_ground) */
#include "platform/ui_text.h"               /* PC (2026-11-16): the interface's language (pcNextView's notice) */

/* The height of the other cars' boxes the in-car cameras test against
 * (isViewBlocked, isInsideCar: a point on each side face this far up).
 * Lupo 2: 1.5 m, so tall vehicles (the Pod, 1.68 m) count fully (L2
 * 0x0020CDF0 / 0x0020D180, cameras_sound.md C7); GT3: 1.0 m. */
#if ENH_CAMERA_OCCLUSION_BOX
#define OCCLUSION_BOX_HEIGHT 1.5f
#else
#define OCCLUSION_BOX_HEIGHT 1.0f
#endif
// DATA_START 0x0033E030
// DATA_START 0x003529C8

/* The camera mounts' names (by mount index, as a car file lists them);
 * nothing in the program reads the table.  Under -G8 the names of 8 bytes
 * or less are in .sdata (0x003529C8..), the others in .rodata (0x0033E040..),
 * each written just before the table.  PC (2026-11-16): the interface's
 * language - the names marked N_ (a mark only: the same strings), pcNextView
 * names the camera over the picture by them, translated. */
const CameraMountName g_0033E100[21] = {    /* 0x0033E100 */
    {  0, N_("Driver") },       {  1, N_("Chase") },        {  2, N_("reserved") },
    {  3, N_("Mirror-L") },     {  4, N_("Mirror-R") },     {  5, N_("Nose") },
    {  6, N_("Option-3") },     {  7, N_("Roof") },         {  8, N_("Back") },
    {  9, N_("Tail") },         { 10, N_("Side-L") },       { 11, N_("Side-R") },
    { 12, N_("Option-4") },     { 13, N_("Option-5") },     { 14, N_("FrontWheel-L") },
    { 15, N_("FrontWheel-R") }, { 16, N_("RearWheel-L") },  { 17, N_("RearWheel-R") },
    { 18, N_("Option-1") },     { 19, N_("Option-2") },     { -1, "" }
};
int g_00352A0C = 20;                        /* 0x00352A0C (? the table's count) */

/* the box face normals (x, y, z, 0): -z, -x, +z, +x, +y, -y */
const f32 g_0033E1A8[6][4] = {              /* 0x0033E1A8 isViewBlocked's */
    { 0.0f, 0.0f, -1.0f, 0.0f }, { -1.0f, 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f, 0.0f },
    { 1.0f, 0.0f, 0.0f, 0.0f },  { 0.0f, 1.0f, 0.0f, 0.0f },  { 0.0f, -1.0f, 0.0f, 0.0f }
};
const f32 g_0033E208[6][4] = {              /* 0x0033E208 isInsideCar's */
    { 0.0f, 0.0f, -1.0f, 0.0f }, { -1.0f, 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f, 0.0f },
    { 1.0f, 0.0f, 0.0f, 0.0f },  { 0.0f, 1.0f, 0.0f, 0.0f },  { 0.0f, -1.0f, 0.0f, 0.0f }
};

/* getMountDirection's / findMountsToward's mask by 45-degree sector */
s64 g_002DB7B0[10] = { 1, 9, 8, 10, 2, 6, 4, 5, 1, 9 };    /* 0x002DB7B0 */

/* ? name.  Mounts 0..4 of a car file with fewer than two mounts: the camera
 * point on the car (applyMountView) and a word that nothing reads (1 for
 * mounts 1 and 2). */
struct DefaultMount {
    f32 x, y, z;                            /* 0x0 */
    int unk_0C;                             /* 0xC */
};
static const DefaultMount g_0033E370[5] = { /* 0x0033E370 */
    { 0.0f, -0.8f, 0.0f, 0 }, { 0.0f, -1.5f, -6.0f, 1 }, { 0.0f, -1.5f, -6.0f, 1 },
    { 0.0f, -0.8f, 0.0f, 0 }, { 0.0f, -0.8f, 0.0f, 0 }
};

static const f32 RAD_TO_DEG  = 57.29578f;  /* 0x42652EE1 */
static const f32 RAD_TO_DEG_B = 57.295776f; /* 0x42652EE0: the box yaw's (one bit less) */
static const f32 KMH_10       = 2.7777777f; /* m/s */
static const f32 KMH_120      = 33.333332f;
static const f32 KMH_10_SQ    = 7.7160487f; /* (10 km/h)^2 */
static const f32 KMH_80_SQ    = 493.82712f; /* (80 km/h)^2 */

// FUNC 0x001FF788 CameraShake::reset
void CameraShake::reset()
{
    target = 0.0f;
    pos = 0.0f;
    vel = 0.0f;
}

/* a new random target (-128..128) with probability 0.4; each draw first
 * steps the generator once more */
// FUNC 0x001FF798 CameraShake::pickTarget
void CameraShake::pickTarget(Random *rng)
{
    rng->next();
    if (rng->next01() < 0.4f) {
        rng->next();
        target = rng->next01() * 256.0f - 128.0f;
    }
}

// FUNC 0x001FF818 CameraShake::getValue
f32 CameraShake::getValue()
{
    f32 v = pos * 0.0078125f;               /* / 128 */

    if (v < -1.0f)
        v = -1.0f;
    if (v > 1.0f)
        v = 1.0f;
    return v;
}

/* a spring (stiffness gain, damping 10) at 60 steps a second */
// FUNC 0x001FF860 CameraShake::update
void CameraShake::update(Random *rng, f32 gain)
{
    pickTarget(rng);
    vel = vel + (gain * (target - pos) + vel * -10.0f) / 60.0f;
    pos = pos + vel / 60.0f;
}

/* PC: the road under the chase camera's eye (its free orbit, update): the
 * runway's height there (RaceCourse::getContact; the sector the hint, kept),
 * none off the road. */
struct PcFreecamGround {
    RaceCourse *course;
    int sector;
};

static int pc_freecam_ground(void *ctx, f32 x, f32 y, f32 z, f32 *groundY)
{
    PcFreecamGround *g = (PcFreecamGround *)ctx;
    RunwayContact c;
    memset(&c, 0, sizeof(c));
    c.x = x;
    c.y = y;
    c.z = z;
    c.sector = g->sector;
    g->course->getContact(&c);
    if (c.offRoad != 0)
        return 0;
    g->sector = c.sector;
    *groundY = c.height;
    return 1;
}

/* PC: the player's camera stick for the chase camera's orbit and the cockpit
 * camera's look (game/app/pc_freecam.h): the chase camera controls (the
 * launcher's; by default the right stick), from the device the player drives
 * with - the pad's on the axes the controller settings leave the right stick
 * free (throttle / brake can be put on it), or the wheel's; 0 without. */
static void pc_camera_stick(RaceInput *input, f32 *stickX, f32 *stickY)
{
    *stickX = 0.0f;
    *stickY = 0.0f;
    const int padType = input != NULL ? input->rawPSType() : PS_PAD_NONE;
    if (padType == PS_PAD_ANALOG || padType == PS_PAD_DUALSHOCK || padType == PS_PAD_DUALSHOCK2) {
        int takenX = 0, takenY = 0;
        ControllerConfig *cfg = input->getControllerConfig();
        for (int f = 0; cfg != NULL && f < KEY_FUNC_COUNT; f++) {
            const int axis = cfg->keyAxis(CONFIG_ANALOG, input->port, f);
            if (axis < 0)
                continue;
            takenX |= (axis & 0xFF) == CHAN_RSTICK_X;
            takenY |= (axis & 0xFF) == CHAN_RSTICK_Y;
        }
        pc_freecam_stick(input->port, 0, takenX, takenY, stickX, stickY);
    } else if (input != NULL && padType == PS_PAD_NONE && input->rawUSBType() != USB_WHEEL_NONE) {
        pc_freecam_stick(input->port, 1, 0, 0, stickX, stickY);
    }
}

// FUNC 0x001FF8E8 OnBoardCamera::OnBoardCamera
/* VariableCameraBase::VariableCameraBase(), rng (Random's inline
 * constructor: 0x87654321), the vptr; then type 0 (setType inline) */
OnBoardCamera::OnBoardCamera()
{
    /* implicit: VariableCameraBase (the base constructor) */
    fov = 60.0f;
    car = NULL;
    mount = 0;
    type = 0;
    trueType = 0;
    pcCockpit = 0;                          /* PC */
    pcCustom = 0;                           /* PC (2026-11-14) */
}

// FUNC 0x001FF948 OnBoardCamera::~OnBoardCamera
/* the egcs code passes its flags straight on to
 * VariableCameraBase::~VariableCameraBase (CameraBase does the delete) */
OnBoardCamera::~OnBoardCamera()
{
    /* implicit: ~VariableCameraBase (the base destructor) */
}

/* attach to the target's car; mount 0 */
// FUNC 0x001FF970 OnBoardCamera::virtual_8
void OnBoardCamera::virtual_8(CameraTargetInfo *target)
{
    f32 bank;

    car = NULL;
    changeable = 1;
    mount = 0;
    pcCockpit = 0;                          /* PC */
    pcCustom = 0;                           /* PC (2026-11-14) */
    reverse = 0;
    drawMirror = 0;
    mountFlag2 = 0;
    car = target->car[0];
    carIndex = target->index[0];
    setMount(MOUNT_SWAY);
    gamma[0] = 1.0f;
    zoom[0] = 1.0f / tanf((fov * 0.5f * 6.2831855f) / 360.0f);
    bankForcibly = 0.0f;                    /* stored as the word 0 and read back */
    bank = bankForcibly;
    counter = 0;
    targetLength[0] = 100.0f;
    springX.reset(bank, bank);
    springY.reset(bank, bank);
    shakeX.reset();
    shakeY.reset();
}

/* PC (2026-11-08, user report: "Some of Player 1's camera data is fed to
 * player 2's camera.  I see the slight bounces from player 1's car get
 * copied to player 2's camera and player 2's camera is much stiffer because
 * it's not getting its own movement data").  The original's: CameraManager::
 * init(org, 0) (T129) gives every view car 0 and attaches every view's
 * onboard camera to it (virtual_8: `car'); RaceSplitBattle::virtual_8 (T137)
 * then moves view 2's target to car 1 with CameraTargetInfo::setCar alone.
 * The view follows the target (applyMountView(target->car[0])), but update
 * read the springs, the head shake and the sway from `car''s state - player
 * 1's car - and the mount list, the view button's mounts and the cockpit
 * camera's head count from its model.  Here a camera whose target car is
 * not its car takes that car: its springs, shakes and sways start again from
 * the car's state (update's first-tick resets, without making this tick a
 * cut), its mount stays unless the car has no such mount (the bumper view
 * then), a second cockpit becomes the first on a car with one driver. */
void OnBoardCamera::pcFollowTarget(CameraTargetInfo *target)
{
    RaceEntryCar *viewed = target->car[0];
    if (viewed == NULL || viewed == car || viewed->state == NULL)
        return;
    car = viewed;
    carIndex = target->index[0];
    RaceCarState *st = car->state;
    springX.reset(0.0f, st->st.accelY * 0.5f);
    springY.reset(0.0f, st->st.heaveAccel + st->st.heaveAccel);
    shakeX.reset();
    shakeY.reset();
    swayY.reset(0.0f, 0.0f);
    swayX.reset(0.0f, 0.0f);
    pc_customcam_shake_reset(this);         /* PC (2026-11-14) */
    if (mount >= 2 && mount >= car->model.data->info->mountCount)
        setMount(MOUNT_SWAY);
    if (pcCockpit == 2 && pc_headcam_count(car) < 2)
        pcCockpit = 1;
    /* PC (2026-11-14): a custom camera this car has not (the other car's
     * model's own): the bumper view; one it has keeps its type's mount */
    if (pcCustom != 0)
        pcSetCustom(pcCustom <= pc_customcam_count(car) ? pcCustom : 0);
}

/* The update: springs, shake and sway from the car's state and the
 * target's speed; the mount buttons; look-back; then the view matrix
 * (applyMountView into `matrix').  0 on the first update (no interpolation).
 * progress is not used. */
// FUNC 0x001FFA70 OnBoardCamera::update
int OnBoardCamera::update(RaceOrganization *org, RaceInput *input, CameraTargetInfo *target,
                              CarProgress *progress)
{
    pcFollowTarget(target);                 /* PC (2026-11-08): the view's own car (above) */
    f32 vx, vy, vz;                         /* sp+0x00 the saved position, then the velocity */
    f32 nx, ny, nz;                         /* sp+0x0C (scalars: retail packs them 4-aligned) */
    f32 speedSq = 1.0f;
    int keep = 1;
    RaceCarState *st = car->state;
    int replayMode = 1;
    u32 pressed = 0;
    int lookBack = 0;
    u8 wasReverse;
    int intr;

    if (input != NULL) {
        pressed = input->getButtonsOn();
        replayMode = input->replayMode;
        lookBack = (int)input->getButtons() >> 16;         /* ONBOARD_BTN_LOOK_BACK (sra; */
        lookBack &= 1;                                      /* the shifted word is stored first) */
    }
    /* PC (2026-11-14): a custom camera (game/app/pc_customcam.h) is a live
     * race's view: a replay's onboard camera shows the car's mounts, and a
     * camera the car has no more (deleted from the other player's view) is
     * left for the bumper view */
    if (pcCustom != 0 && ((input != NULL && replayMode != 0) || pcCustom > pc_customcam_count(car))) {
        pcSetCustom(0);
        doppler = 0;
    }
    if (target->getSavedPosition(&vx, &vy, &vz)) {
        target->getCarPosition(0, &nx, &ny, &nz);
        vx = (nx - vx) * 60.0f;
        vy = (ny - vy) * 60.0f;
        vz = (nz - vz) * 60.0f;
        speedSq = vx * vx + vy * vy + vz * vz;
    }
    pc_vr_shake_tick(speedSq);              /* PC VR: the headset's vibration (game/app/pc_vr.h) */
    if (counter == 0) {
        keep = 0;                           /* first: else CSE takes counter's 1 from keep */
        counter = 1;
        springX.reset(0.0f, st->st.accelY * 0.5f);
        springY.reset(0.0f, st->st.heaveAccel + st->st.heaveAccel);
        shakeX.reset();
        shakeY.reset();
        swayY.reset(0.0f, 0.0f);
        swayX.reset(0.0f, 0.0f);
        pc_customcam_shake_reset(this);     /* PC (2026-11-14) */
    }
    springX.update(st->st.accelY * 0.5f);
    springY.update(st->st.heaveAccel + st->st.heaveAccel);
    speedFactor = 0.0f;
    if (pcCustom != 0) {
        /* PC (2026-11-14): a custom camera (game/app/pc_customcam.h) takes a
         * share of every way the fixed views move with the car - its
         * rigidities - so all of them run for it: the bumper view's sway and
         * speed factor (below, as mount 0's), the in-car mounts' shake by the
         * speed (shakeX / shakeY, as the other mounts'), and the shake by
         * the wheels' slip - the bumper view's gain, which the cockpit
         * camera shows - in a pair of its own (pc_customcam_shake) */
        f32 slip = 0.0f;
        f32 speed;
        f32 excess;
        int w;

        for (w = 0; w < 4; w++) {
            slip += boot_fabsf(st->st.wheel[w].slip * 0.3f);
            slip += boot_fabsf(st->st.tyreForce[w].slipAngle / 1.5707964f);
        }
        if (slip < 0.0f)
            slip = 0.0f;
        if (slip > 3.0f)
            slip = 3.0f;
        pc_customcam_shake(this, &rng, (slip * 115.0f) / 3.0f);
        speed = boot_sqrtf_v(speedSq);
        if (speed < KMH_10)
            speed = KMH_10;
        if (speed > KMH_120)
            speed = KMH_120;
        speedFactor = speed / KMH_120;
        rng.next();
        if (rng.next01() < 0.5f) {
            rng.next();
            swayY.update((rng.next01() - 0.5f) * slip * 250.0f);
            rng.next();
            swayX.update((rng.next01() - 0.5f) * slip * 250.0f);
        } else {
            swayY.update(0.0f);
            swayX.update(0.0f);
        }
        excess = speedSq - KMH_10_SQ;
        if (excess < 0.0f)
            excess = 0.0f;
        if (excess > KMH_80_SQ)
            excess = KMH_80_SQ;
        shakeX.update(&rng, (excess * 115.0f) / KMH_80_SQ);
        shakeY.update(&rng, (excess * 115.0f) / KMH_80_SQ);
    } else if (getMount() == MOUNT_SWAY) {
        f32 slip = 0.0f;                    /* the wheels' slip ratios and angles, 0..3 */
        f32 gain;
        f32 speed;
        int w;

        for (w = 0; w < 4; w++) {
            slip += boot_fabsf(st->st.wheel[w].slip * 0.3f);
            slip += boot_fabsf(st->st.tyreForce[w].slipAngle / 1.5707964f);
        }
        if (slip < 0.0f)
            slip = 0.0f;
        if (slip > 3.0f)
            slip = 3.0f;
        gain = (slip * 115.0f) / 3.0f;
        shakeX.update(&rng, gain);
        shakeY.update(&rng, gain);
        speed = boot_sqrtf_v(speedSq);      /* retail keeps it after the shake updates */
        if (speed < KMH_10)
            speed = KMH_10;
        if (speed > KMH_120)
            speed = KMH_120;
        speedFactor = speed / KMH_120;
        rng.next();
        if (rng.next01() < 0.5f) {
            rng.next();
            swayY.update((rng.next01() - 0.5f) * slip * 250.0f);
            rng.next();
            swayX.update((rng.next01() - 0.5f) * slip * 250.0f);
        } else {
            swayY.update(0.0f);
            swayX.update(0.0f);
        }
    } else {
        f32 excess = speedSq - KMH_10_SQ;
        f32 gain;

        if (excess < 0.0f)
            excess = 0.0f;
        if (excess > KMH_80_SQ)
            excess = KMH_80_SQ;
        gain = (excess * 115.0f) / KMH_80_SQ;
        shakeX.update(&rng, gain);
        shakeY.update(&rng, gain);
    }

    /* PC (2026-11-14): the custom cameras' editor and the F2 menu's requests
     * (game/app/pc_customcam.h; a live race): a new camera is put in view, a
     * deleted one leaves it for the bumper view, a changed type takes its
     * mount; while a camera is edited the camera controls are the editor's
     * and the view button leaves it in view */
    int pcEdit = 0;
    if (replayMode == 0 && input != NULL) {
        f32 editX = 0.0f, editY = 0.0f;
        PcCustomSource shown;
        const int editing = pc_customcam_editing();
        if (editing) {
            pc_camera_stick(input, &editX, &editY);
            if (pcCustom == 0)
                pcDescribeView(&shown);
        }
        int index = pcCustom;
        pcEdit = pc_customcam_edit(this, car, input->port, &index, editing && pcCustom == 0 ? &shown : NULL, editX,
                                   editY);
        if (index != pcCustom || (pcEdit & PC_CUSTOM_TYPE)) {
            pcSetCustom(index);
            doppler = 0;
        }
    }

    switch (replayMode) {
    case 0:
        if ((pressed & ONBOARD_BTN_VIEW) && !(pcEdit & PC_CUSTOM_LOCK)) {
            /* PC (the cockpit camera too: pc_headcam.h; 2026-11-14: a car
             * with custom cameras too) */
            if (pc_extra_cameras() || pc_headcam_on() || pcCustom != 0 || pc_customcam_count(car) > 0) {
                pcNextView();
            } else {
            mount = mount + 1;
            if (mount >= 2)
                mount = MOUNT_SWAY;
            pcCockpit = 0;                      /* PC (the setting turned off in a race) */
            }
            doppler = 0;
        }
        break;
    case 1:
    case 2:
    case 3:
        /* PC (2026-11-14): never under a custom camera, a live race's view
         * (a replay's input drops it above).  A race's first update runs
         * before the race gives the view its input (RaceBase::virtual_8:
         * cm->update, then views[0].input), as replay mode 1; in a race
         * without a countdown (a licence test) the onboard camera is the
         * view's then, and on a car of fewer than MOUNT_MIN_SELECTABLE mounts
         * (12 of the US cars) this put mount 0 under a chase type custom
         * camera: the bumper view's HUD, sounds and mirror (user report:
         * "starting a license test while a custom camera was last selected
         * can get the HUD style wrong").  The game's own views keep the
         * original's: its chase view becomes the bumper view on that tick. */
        if (changeable && pcCustom == 0) {
            if (car->model.data->info->mountCount < MOUNT_MIN_SELECTABLE) {
                setMount(MOUNT_SWAY);
            } else if (pressed & ONBOARD_BTN_LEFT) {
                stepMount(1, MOUNT_SELECTABLE);
                doppler = 0;
            } else if (pressed & ONBOARD_BTN_RIGHT) {
                stepMount(-1, MOUNT_SELECTABLE);
                doppler = 0;
            }
        }
        break;
    }

    /* PC: the debug camera forces the chase view of a live race's player 1
     * (the car not hidden as in the bumper view), the player's own back
     * after it (game/app/pc_debugcam.h) */
    {
        const int m = pc_debugcam_mount(this, replayMode == 0 && (input == NULL || input->port == 0), mount,
                                        MOUNT_CHASE);
        if (m != mount) {
            mount = m;
            doppler = 0;
        }
    }

    wasReverse = reverse;
    reverse = 0;
    if (lookBack)
        reverse = 1;
    if (wasReverse != reverse)
        doppler = 0;
    drawMirror = 0;                         /* a bit-field store: the vtable loads pass it */
    /* PC: the rear view per camera (game/app/pc_detail.h): the bumper
     * camera's as the original's unless turned off, the chase camera's
     * added when turned on */
    if (getMount() == MOUNT_SWAY && reverse == 0 && pc_rear_view_bumper())
        drawMirror = 1;
    if (getMount() == MOUNT_CHASE && reverse == 0 && pc_rear_view_chase())     /* PC */
        drawMirror = 1;

    intr = intr_suspend();
    pgl_push_matrix();
    vu0_load_identity_regs();
    applyMountView(target->car[0]);
    pgl_get_matrix(&matrix[0][0]);
    pgl_pop_matrix();
    intr_resume(intr);

    /* PC: the chase camera's free orbit (game/app/pc_freecam.h; user
     * request): the player's chase camera controls (the launcher's; by
     * default the right stick), from the device the player drives with - the
     * pad's on the axes the controller settings leave the right stick free
     * (throttle / brake can be put on it), or the wheel's; the road under the
     * eye from the runway */
    PcCustomView pcView;
    const int pcCustomOn = pcCustom != 0 && pc_customcam_view(target->car[0], pcCustom, &pcView);
    if (pcCustomOn && reverse == 0) {
        /* PC (2026-11-14): a custom camera (game/app/pc_customcam.h): its own
         * angle is the swing's rest (user: "The existing chase cam angle
         * setting should only apply to the base chase cam.  The user-made
         * ones get their own"); the camera controls swing a chase type
         * round the car as the chase camera's do, and look round from the
         * eye of an onboard type as the cockpit camera's do - not while its
         * editor has them */
        f32 stickX = 0.0f, stickY = 0.0f;
        if (!(pcEdit & PC_CUSTOM_STICK))
            pc_camera_stick(input, &stickX, &stickY);
        RaceCarState *vs = target->car[0]->state;
        const f32 pivot[3] = {car_view_x(vs, 0.0f), car_view_y(vs, 0.0f), car_view_z(vs, 0.0f)};
        const int orbit = !pcView.onboard;
        if (orbit || pcView.angle != 0.0f) {
            PcFreecamGround ground = {org != NULL ? org->course : NULL, vs->st.sector};
            /* (PC 2026-11-14: in the view's own axes - its ANGLE the same
             * way on every car, above 0 looking down as the chase camera's;
             * by the eye's side it flipped where the eye sat over the car's
             * middle: user report, pc_freecam.h) */
            pc_freecam_apply(&pcCustom, &matrix[0][0], pivot, orbit ? stickX : 0.0f, orbit ? stickY : 0.0f,
                             orbit && ground.course != NULL ? pc_freecam_ground : NULL, &ground,
                             car_ground_view_y(vs, 0.0f), pcView.angle, 1);
        }
        if (!orbit)
            pc_freecam_look(&fov, &matrix[0][0], stickX, stickY);
    } else if (!pcCustomOn && getMount() == MOUNT_CHASE && reverse == 0) {
        f32 stickX, stickY;
        pc_camera_stick(input, &stickX, &stickY);
        RaceCarState *vs = target->car[0]->state;
        const f32 pivot[3] = {car_view_x(vs, 0.0f), car_view_y(vs, 0.0f), car_view_z(vs, 0.0f)};
        PcFreecamGround ground = {org != NULL ? org->course : NULL, vs->st.sector};
        /* (PC 2026-11-14: [Graphics] ChaseAngle is this camera's alone) */
        pc_freecam_apply(this, &matrix[0][0], pivot, stickX, stickY, ground.course != NULL ? pc_freecam_ground : NULL,
                         &ground, car_ground_view_y(vs, 0.0f), pc_freecam_chase_rest());
    }
    if (pcCustomOn) {
        /* (the eye as shown, in the car's frame - the matrix from the world
         * to it, the car's whole rotation: what of the car the eye sits in
         * is not drawn, pc_customcam_hide) */
        RaceCarState *vs = target->car[0]->state;
        f32 frame[16];
        intr = intr_suspend();
        pgl_push_matrix();
        vu0_load_identity_regs();
        vu0_rotate_z(car_roll(vs, 0.0f) * RAD_TO_DEG);
        vu0_rotate_x(-car_pitch(vs, 0.0f) * RAD_TO_DEG);
        vu0_rotate_y(-car_yaw(vs, 0.0f) * RAD_TO_DEG);
        pgl_translate(-car_view_x(vs, 0.0f), -car_view_y(vs, 0.0f), -car_view_z(vs, 0.0f));
        pgl_get_matrix(frame);
        pgl_pop_matrix();
        intr_resume(intr);
        pc_customcam_shown(this, &matrix[0][0], frame);
    }
    /* PC (2026-11-07): the cockpit camera's look round on the same controls
     * (game/app/pc_headcam.h): the view turned about its eye, back to the
     * front when let go; not while looking back (the view is turned round).
     * PC (2026-11-08): calibrating (the F2 menu's CALIBRATE COCKPIT CAMERA,
     * platform/cockpit_osd.h), the controls move the eye and tilt the view
     * instead, the look back at the front. */
    if (pcIsCockpit() && pc_headcam_on() && reverse == 0) {
        f32 stickX, stickY;
        pc_camera_stick(input, &stickX, &stickY);
        /* (the calibrating player's camera alone: the other player of a
         * split screen keeps the look round) */
        if (pc_headcam_calibrating() &&
            pc_headcam_calibrate(target->car[0], pcCockpit, input != NULL ? input->port : -1, stickX, stickY)) {
            stickX = 0.0f;
            stickY = 0.0f;
        }
        pc_freecam_look(&pcCockpit, &matrix[0][0], stickX, stickY);
    } else if (pc_headcam_calibrating()) {
        /* (the panel: not the cockpit view) */
        pc_headcam_calibrate(NULL, 0, input != NULL ? input->port : -1, 0.0f, 0.0f);
    }
    return keep;
}

/* Builds the view of the current mount on car `viewed' into the current
 * matrix (from the car's position and angles at t = 0). */
// FUNC 0x00200038 OnBoardCamera::applyMountView
void OnBoardCamera::applyMountView(RaceEntryCar *viewed)
{
    RaceCarState *s = viewed->state;
    f32 x = car_view_x(s, 0.0f);
    f32 y = car_view_y(s, 0.0f);
    f32 z = car_view_z(s, 0.0f);

    /* PC (2026-11-14): a custom camera (game/app/pc_customcam.h): its eye in
     * the car's frame (minus the point, as a mount's), its pan (about the
     * car's up axis) and then its tilt (about its own right axis: up
     * positive, as the cockpit camera's), and of each way the fixed views
     * move with the car the share its rigidity leaves (loose: 1 - the
     * rigidity) - the in-car mounts' shake (by the speed) and the cockpit
     * camera's (by the wheels' slip: pc_customcam_shake) and the bumper
     * view's sway, turned about the eye; the chase view's lag behind the
     * car's turn and pitch taken out of the car's rotation, and its level
     * horizon (the car's roll by the rigidity); the in-car mounts' springs
     * on the car's place.  Looking back turns it round as the fixed views
     * are. */
    PcCustomView cv;
    if (pcCustom != 0 && pc_customcam_view(viewed, pcCustom, &cv)) {
        f32 slipX, slipY;
        f32 yaw, pitch, roll;

        pc_customcam_shake_get(this, &slipX, &slipY);
        vu0_rotate_x(shakeX.getValue() * cv.loose[PC_RIGID_SPEED_SHAKE] + slipX * cv.loose[PC_RIGID_SLIP_SHAKE]);
        vu0_rotate_y((shakeY.getValue() * cv.loose[PC_RIGID_SPEED_SHAKE] + slipY * cv.loose[PC_RIGID_SLIP_SHAKE]) *
                     0.8f);
        vu0_rotate_x(swayX.clampedPos() * 8.0f * speedFactor * cv.loose[PC_RIGID_SWAY]);
        vu0_rotate_y(swayY.clampedPos() * 2.0f * speedFactor * cv.loose[PC_RIGID_SWAY]);
        vu0_rotate_x(-cv.tilt);
        vu0_rotate_y(cv.pan);
        pgl_translate(-cv.eye[0], -cv.eye[1], -cv.eye[2]);
        yaw = car_yaw(s, 0.0f) - car_lag_yaw(s, 0.0f) * cv.loose[PC_RIGID_TURN];
        pitch = car_pitch(s, 0.0f) - car_lag_pitch(s, 0.0f) * cv.loose[PC_RIGID_PITCH];
        roll = car_roll(s, 0.0f) * (1.0f - cv.loose[PC_RIGID_ROLL]);
        if (reverse) {
            pitch = -pitch;
            roll = -roll;
            yaw += 3.1415927f;
        }
        vu0_rotate_z(roll * RAD_TO_DEG);
        vu0_rotate_x(-pitch * RAD_TO_DEG);
        vu0_rotate_y(-yaw * RAD_TO_DEG);
        pgl_translate(-(x + springX.clampedPos() * cv.loose[PC_RIGID_SIDE]),
                      -(y + springY.clampedPos() * cv.loose[PC_RIGID_BUMPS]), -z);
        targetLength[0] = 100.0f;
        zoom[0] = 1.0f / tanf((cv.fov * 0.5f * 6.2831855f) / 360.0f);
        return;
    }

    /* PC (2026-11-07): the cockpit camera (game/app/pc_headcam.h): the eye in
     * the driver's head (the car's frame: minus the point, as a mount's), the
     * in-car mounts' head shake and springs, the car's whole rotation as
     * mount 0's (the head rides with the car), its own field of view; looking
     * back turns it round as the fixed views do */
    if (pcIsCockpit() && pc_headcam_on()) {
        f32 eye[3];
        f32 yaw, pitch, roll;

        pc_headcam_eye(viewed, pcCockpit, eye);     /* (the slot: 1 the driver, 2 a second driver; with the
                                                       model's calibrated shift) */
        /* PC (2026-11-08): the model's calibrated tilt, up positive: the
         * view turns by the opposite of the camera's own turn (vu0RotateX
         * rotates row vectors right-handed about x: nose up for a positive
         * angle, as the car's pitch undone below by -pitch) */
        vu0_rotate_x(-pc_headcam_tilt(viewed, pcCockpit));
        /* PC VR: in the headset the view's shake is the player's to set ([VR] Shake, HeadShake: the game's own shake
         * times the strength, a road / engine vibration on top, none when off; the cabin's shake is drawCars's); on the
         * flat screen gain is 1 and the vibration 0: as ever.  An axis is held to 3 degrees. */
        f32 shakeGain, vibPitch, vibYaw, vibRoll;
        pc_vr_shake_get(&shakeGain, &vibPitch, &vibYaw, &vibRoll);
        vu0_rotate_x(pc_vr_shake_cap(shakeX.getValue() * shakeGain + vibPitch));
        vu0_rotate_y(pc_vr_shake_cap(shakeY.getValue() * 0.8f * shakeGain + vibYaw));
        if (vibRoll != 0.0f)
            vu0_rotate_z(pc_vr_shake_cap(vibRoll));
        x += springX.clampedPos() * shakeGain;      /* (the springs are the shake's: the car's body on its springs) */
        y += springY.clampedPos() * shakeGain;
        pgl_translate(-eye[0], -eye[1], -eye[2]);
        yaw = car_yaw(s, 0.0f);
        pitch = car_pitch(s, 0.0f);
        roll = car_roll(s, 0.0f);
        if (reverse) {
            pitch = -pitch;
            roll = -roll;
            yaw += 3.1415927f;
        }
        vu0_rotate_z(roll * RAD_TO_DEG);
        vu0_rotate_x(-pitch * RAD_TO_DEG);
        vu0_rotate_y(-yaw * RAD_TO_DEG);
        pgl_translate(-x, -y, -z);
        targetLength[0] = 100.0f;
        zoom[0] = 1.0f / tanf((pc_headcam_fov() * 0.5f * 6.2831855f) / 360.0f);
        return;
    }

    switch (getMount()) {
    case MOUNT_SWAY: {
        f32 v;

        vu0_rotate_x(swayX.clampedPos() * 8.0f * speedFactor);
        v = swayY.clampedPos();
        vu0_rotate_y((v + v) * speedFactor);
    }
        /* fall through */
    case MOUNT_CHASE: {
        CarModelInfo *info = viewed->model.data->info;
        f32 yaw, pitch, roll;

        if (info->mountCount >= 2) {
            CameraMount *mt = &info->mounts[getMount()];
            pgl_translate(mt->x, mt->y, mt->z);
        } else {
            const DefaultMount *d = &g_0033E370[getMount()];
            pgl_translate(d->x, d->y, d->z);
        }
        yaw = car_yaw(s, 0.0f);
        pitch = car_pitch(s, 0.0f);
        roll = car_roll(s, 0.0f);
        if (getMount() > MOUNT_SWAY) {
            yaw -= car_lag_yaw(s, 0.0f);
            pitch -= car_lag_pitch(s, 0.0f);
            roll = 0.0f;
        }
        if (reverse) {
            pitch = -pitch;
            roll = -roll;
            yaw += 3.1415927f;
        }
        vu0_rotate_z(roll * RAD_TO_DEG);
        vu0_rotate_x(-pitch * RAD_TO_DEG);
        vu0_rotate_y(-yaw * RAD_TO_DEG);
        pgl_translate(-x, -y, -z);
        targetLength[0] = 100.0f;
        zoom[0] = 1.0f / tanf((fov * 0.5f * 6.2831855f) / 360.0f);
        break;
    }
    default: {
        CarModelInfo *info = viewed->model.data->info;
        CameraMount *mt = &info->mounts[getMount()];
        f32 yaw, pitch;

        if (!(mt->flags & MOUNT_NO_SHAKE)) {
            vu0_rotate_x(shakeX.getValue());
            vu0_rotate_y(shakeY.getValue() * 0.8f);
            x += springX.clampedPos();
            y += springY.clampedPos();
        }
        vu0_rotate_z(mt->rotZ);
        vu0_rotate_x(mt->rotX);
        vu0_rotate_y(mt->rotY);
        pgl_translate(mt->x, mt->y, mt->z);
        yaw = car_yaw(s, 0.0f);
        pitch = car_pitch(s, 0.0f);
        vu0_rotate_z(car_roll(s, 0.0f) * RAD_TO_DEG);
        vu0_rotate_x(-pitch * RAD_TO_DEG);
        vu0_rotate_y(-yaw * RAD_TO_DEG);
        pgl_translate(-x, -y, -z);
        targetLength[0] = 100.0f;
        zoom[0] = mt->zoom;
        break;
    }
    }
}

/* (? how to draw the followed car): mount 0 -> 0, mount 1 -> 2, other
 * mounts -> MOUNT_DRAW_INSIDE of the mount's flags */
// FUNC 0x002003E8 OnBoardCamera::virtual_9
int OnBoardCamera::virtual_9()
{
    int drawMode = 0;

    switch (getMount()) {
    case MOUNT_SWAY:
        break;
    case MOUNT_CHASE:
        drawMode = 2;
        break;
    default:
        if (mountFlag2)
            drawMode = 1;
        break;
    }
    return drawMode;
}

// FUNC 0x00200450 OnBoardCamera::getEffect
int OnBoardCamera::getEffect()
{
    return 0;
}

/* as VariableCameraBase::setProjection, with the focal scaled by the mount's
 * zoom (bottom before top: pgl_frustum's order) */
// FUNC 0x00200458 OnBoardCamera::setProjection
void OnBoardCamera::setProjection(const CameraProjection *proj)
{
    f32 z = zoom[0];

    pgl_frustum(proj->left, proj->right, proj->bottom, proj->top, proj->nearClip, proj->farClip,
                proj->focal * z);
    setLodBias(zoom[0]);
}

// FUNC 0x002004B0 OnBoardCamera::applyView
void OnBoardCamera::applyView()
{
    pgl_mult_matrix(&matrix[0][0]);
}

// FUNC 0x002004D0 OnBoardCamera::setMount
void OnBoardCamera::setMount(int m)
{
    mount = m;
    if (car != NULL)
        setMountChecked(m);
}

// FUNC 0x00200500 OnBoardCamera::setBasicMount
void OnBoardCamera::setBasicMount(int m)
{
    /* PC: with the extra cameras the last race's view, whichever it was (the
     * cockpit view with the cockpit camera too: pc_headcam.h) */
    pcCockpit = 0;
    pcCustom = 0;                           /* PC (2026-11-14) */
    if (m >= MOUNT_PC_CUSTOM) {
        /* PC (2026-11-14): the custom camera that was in view (its number
         * among the car's: the global ones come first, the same for every
         * car); the bumper view on a car without it */
        pcSetCustom(m - MOUNT_PC_CUSTOM + 1);
        if (pcCustom != 0)
            return;
        m = MOUNT_SWAY;
    }
    if ((pc_extra_cameras() || pc_headcam_on()) && m == MOUNT_PC_COCKPIT) {
        pcCockpit = 1;
        m = MOUNT_SWAY;
    } else if (!pc_extra_cameras()) {
    if (!(m < 2))
        m = MOUNT_SWAY;
    }
    setMount(m);
}

/* PC: the view button with the extra cameras on: bumper, chase, cockpit (the
 * bumper's eye, the car file's "Driver"; with the cockpit camera on, the
 * camera in the driver's head: pc_headcam.h), then the car's own mounts in
 * its order, back to the bumper.  A car without mounts has the first three;
 * the cockpit camera alone (the extra cameras off) has the first three.
 * PC (2026-11-14): the car's custom cameras (pc_customcam.h) come after the
 * cockpit view - after the chase view where neither setting gives a cockpit
 * view - the global ones, then its model's. */
void OnBoardCamera::pcNextView()
{
    const char *name;
    const int customs = pc_customcam_count(car);
    const int extras = pc_extra_cameras() && getMountCount() > MOUNT_PC_FIRST_EXTRA;

    if (pcCustom != 0) {
        if (pcCustom < customs) {
            pcSetCustom(pcCustom + 1);
        } else {
            pcSetCustom(0);
            setMount(extras ? (int)MOUNT_PC_FIRST_EXTRA : (int)MOUNT_SWAY);
        }
    } else if (pcCockpit == 1 && pc_headcam_on() && pc_headcam_count(car) >= 2) {
        pcCockpit = 2;                      /* PC (2026-11-07): the second driver's (pc_headcam.h) */
    } else if (pcCockpit) {
        pcCockpit = 0;
        if (customs > 0)
            pcSetCustom(1);
        else
            setMount(extras ? (int)MOUNT_PC_FIRST_EXTRA : (int)MOUNT_SWAY);
    } else if (mount == MOUNT_SWAY) {
        mount = MOUNT_CHASE;
    } else if (mount == MOUNT_CHASE) {
        if (pc_extra_cameras() || pc_headcam_on()) {
            mount = MOUNT_SWAY;
            pcCockpit = 1;
        } else if (customs > 0) {
            pcSetCustom(1);
        } else {
            mount = MOUNT_SWAY;
        }
    } else {
        setMount(mount + 1 < getMountCount() ? mount + 1 : MOUNT_SWAY);
    }
    /* PC (2026-11-16): the interface's language (tr; a custom camera's name
     * is the user's own) */
    if (pcCustom != 0)
        name = pc_customcam_name(car, pcCustom);
    else if (pcCockpit == 2)
        name = tr("Cockpit 2");
    else if (pcCockpit)
        name = tr("Cockpit");
    else if (mount == MOUNT_SWAY)
        name = tr("Bumper");
    else
        name = tr(g_0033E100[mount < 20 ? mount : 20].name);
    pc_camera_notice(name);
}

int OnBoardCamera::pcSavedMount()
{
    if (pcCustom != 0)
        return MOUNT_PC_CUSTOM + pcCustom - 1;  /* PC (2026-11-14) */
    return pcCockpit ? (int)MOUNT_PC_COCKPIT : mount;
}

/* PC (2026-11-14): custom camera `index' of the car put in view, the mount
 * its type's (0 onboard, 1 chase: the fixed view it is to the sounds, the
 * HUD, the mirror and the car's drawing); 0, or one the car has not: out of
 * a custom view to the bumper view. */
void OnBoardCamera::pcSetCustom(int index)
{
    if (index > 0 && car != NULL && index <= pc_customcam_count(car)) {
        pcCustom = index;
        pcCockpit = 0;
        mount = pc_customcam_onboard(car, index) ? MOUNT_SWAY : MOUNT_CHASE;
        mountFlag2 = 0;
        return;
    }
    if (pcCustom != 0) {
        pcCustom = 0;
        mount = MOUNT_SWAY;
        mountFlag2 = 0;
    }
}

/* PC (2026-11-14): the view shown as a custom camera would give it - where
 * its eye is taken from, its type, turn, angle, field of view and the
 * rigidities that leave it just the movement it has: what a new custom
 * camera starts as (pc_customcam.h).  The field of view is the one shown: a
 * fixed view's with the FIELD OF VIEW setting, which a custom camera's own
 * does not take. */
void OnBoardCamera::pcDescribeView(PcCustomSource *out)
{
    CarModelInfo *info = car->model.data->info;
    int i;

    memset(out, 0, sizeof(*out));
    for (i = 0; i < PC_RIGID_COUNT; i++)
        out->rigid[i] = 100;
    out->fov = fov * pc_fov_percent() / 100.0f;
    if (pcIsCockpit() && pc_headcam_on()) {
        /* the cockpit camera: the head, its calibrated tilt, its own field
         * of view; the wheels' slip shakes it, the springs move it */
        out->from = 3;
        out->onboard = 1;
        out->tilt = pc_headcam_tilt(car, pcCockpit);
        if (pcCockpit == 2) {               /* (a second driver's: from the first's head) */
            f32 a[3], b[3];
            pc_headcam_eye(car, 1, a);
            pc_headcam_eye(car, 2, b);
            for (i = 0; i < 3; i++)
                out->pos[i] = b[i] - a[i];
        }
        out->fov = pc_headcam_fov();
        out->rigid[PC_RIGID_SLIP_SHAKE] = 0;
        out->rigid[PC_RIGID_SIDE] = 0;
        out->rigid[PC_RIGID_BUMPS] = 0;
    } else if (mount == MOUNT_SWAY) {
        /* the bumper view (the other cockpit view too): it sways */
        out->from = 1;
        out->onboard = 1;
        out->rigid[PC_RIGID_SWAY] = 0;
    } else if (mount == MOUNT_CHASE) {
        /* the chase view: it lags behind the car's turn and pitch and keeps
         * the horizon level; its angle the setting's */
        out->from = 2;
        out->onboard = 0;
        out->angle = pc_freecam_chase_rest();
        out->rigid[PC_RIGID_TURN] = 0;
        out->rigid[PC_RIGID_PITCH] = 0;
        out->rigid[PC_RIGID_ROLL] = 0;
    } else if (mount < info->mountCount) {
        /* one of the car's own mounts: its point from the car's origin, its
         * turn (rotZ, a bank, is left out), its field of view; the speed
         * shakes it and the springs move it unless the mount says not */
        CameraMount *mt = &info->mounts[mount];

        out->from = 0;
        out->onboard = 1;
        out->pos[0] = -mt->x;
        out->pos[1] = -mt->y;
        out->pos[2] = -mt->z;
        out->pan = mt->rotY;
        while (out->pan > 180.0f)
            out->pan -= 360.0f;
        while (out->pan < -180.0f)
            out->pan += 360.0f;
        out->tilt = -mt->rotX;
        if (mt->zoom > 0.0f)
            out->fov = 2.0f * atan2f(1.0f, mt->zoom) * RAD_TO_DEG * pc_fov_percent() / 100.0f;
        if (!(mt->flags & MOUNT_NO_SHAKE)) {
            out->rigid[PC_RIGID_SPEED_SHAKE] = 0;
            out->rigid[PC_RIGID_SIDE] = 0;
            out->rigid[PC_RIGID_BUMPS] = 0;
        }
    }
}

// FUNC 0x00200520 OnBoardCamera::getMountCount
int OnBoardCamera::getMountCount()
{
    return car->model.data->info->mountCount;
}

/* mounts 0 and 1: front; others: the 45-degree sector of the mount's y
 * rotation (a negative i also takes that path: the original does) */
// FUNC 0x00200538 OnBoardCamera::getMountDirection
int OnBoardCamera::getMountDirection(int i)
{
    CarModelInfo *info = car->model.data->info;
    CameraMount *mt;
    int sector;

    if (i >= info->mountCount)
        return 0;
    if (i < 2) {
        if (i >= 0)
            return MOUNT_DIR_FRONT;
    }
    mt = &info->mounts[i];
    sector = (int)(angle_wrap360(mt->rotY) + 22.5f) / 45;
    return (int)g_002DB7B0[sector < 9 ? sector : 8];
}

/* The mounts (from 2) that face the entry `target' as seen from this car:
 * the sector of the heading to it relative to this car's yaw.  A mount
 * qualifies with any of anyFlags (or anyFlags 0) and all of allFlags.
 * At most max indices into out; returns how many. */
// FUNC 0x002005E8 OnBoardCamera::findMountsToward
int OnBoardCamera::findMountsToward(RaceOrganization *org, int target, int *out, int max,
                                    int anyFlags, int allFlags)
{
    RaceEntryCar *self = car;
    CarModelInfo *info = self->model.data->info;
    int count = info->mountCount;
    RaceEntryBase *entries = org->entries;
    RaceCarState *me;
    RaceCarState *other;
    f32 dx, dy, dz, heading, rel;
    int sector, dir, i, found;

    if (target < 0)
        return 0;
    if (!(target < entries->count))
        return 0;
    me = self->state;
    other = entries->cars[target]->state;
    dx = car_view_x(other, 0.0f) - car_view_x(me, 0.0f);
    dy = car_view_y(other, 0.0f) - car_view_y(me, 0.0f);
    dz = car_view_z(other, 0.0f) - car_view_z(me, 0.0f);
    heading = 360.0f - angle_wrap360(camera_yaw_deg(dx, dy, dz));
    rel = angle_diff(angle_wrap360(car_yaw(me, 0.0f) * RAD_TO_DEG), heading);
    if (rel < 0.0f)
        rel += 360.0f;
    sector = (int)(rel + 22.5f) / 45;
    dir = (int)g_002DB7B0[sector < 9 ? sector : 8];
    found = 0;
    for (i = MOUNT_FIRST_OWN; i < count; i++) {
        CameraMount *mt;

        if (getMountDirection(i) != dir)
            continue;
        if (!(found < max))
            continue;
        mt = &info->mounts[i];
        if ((mt->flags & anyFlags) == 0 && anyFlags != 0)
            continue;
        if ((mt->flags & allFlags) != allFlags)
            continue;
        *out++ = i;
        found++;
    }
    return found;
}

/* The mounts (from 2) whose direction mask meets dirMask, with any of
 * anyFlags (here 0 matches nothing) and all of allFlags. */
// FUNC 0x00200858 OnBoardCamera::findMountsByDirection
int OnBoardCamera::findMountsByDirection(int dirMask, int *out, int max, int anyFlags, int allFlags)
{
    int found = 0;
    int i = MOUNT_FIRST_OWN;
    CarModelInfo *info = car->model.data->info;
    int count = info->mountCount;

    for (; i < count; i++) {
        CameraMount *mt;

        if ((getMountDirection(i) & dirMask) == 0)
            continue;
        if (!(found < max))
            continue;
        mt = &info->mounts[i];
        if ((mt->flags & anyFlags) == 0)
            continue;
        if ((mt->flags & allFlags) != allFlags)
            continue;
        *out++ = i;
        found++;
    }
    return found;
}

/* 1 when p is on the inner side (<= 0) of all six planes */
// FUNC 0x00200950 isInsidePlanes
int isInsidePlanes(const f32 (*planes)[4], const f32 *p)
{
    int i;

    for (i = 0; i < 6; i++) {
        const f32 *pl = planes[i];

        if (pl[0] * p[0] + pl[1] * p[1] + pl[2] * p[2] + pl[3] > 0.0f)
            return 0;
    }
    return 1;
}

// FUNC 0x002009C0 dot3
f32 dot3(const f32 *a, const f32 *b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* Mounts 5 and up: 1 when the mount's point (the inverse of the mount's
 * translated car frame applied to the origin) is off the runway or inside
 * the box of another entry (the boxes grown by 4 / 4 / 5 cm, from the
 * ground to 1 m up; the enhanced engine: OCCLUSION_BOX_HEIGHT, Lupo 2's 1.5 m).  The boxes are in the course frame: (x, y, z) ->
 * (x, z, -y).
 * (92.4%, open: retail keeps the facePoint stores in source order; our
 * sched1 moves the stores at which a value dies (hw, rear, -hw, -front)
 * first (register-weight tie-break), and no statement order reproduces
 * retail's (worked through in ref/matching/u089/notes.md); retail also keeps
 * &box->statePos in a register (pos[] / yaw through it).  Tried: rows as
 * do { } while (0) macros, row pointers, an inline row setter, volatile.) */
// FUNC 0x002009F0 OnBoardCamera::isViewBlocked
int OnBoardCamera::isViewBlocked(RaceOrganization *org)
{
    f32 cam[4];                             /* sp+0x00 */
    f32 p[3];                               /* sp+0x10 */
    f32 plane[4];                           /* sp+0x20 */
    f32 planes[6][4];                       /* sp+0x30 */
    f32 facePoint[6][4];                    /* sp+0x90 */
    f32 world[6][4];                        /* sp+0xF0 */
    int sector;                             /* sp+0x150 */
    RaceEntryCar *self;
    CameraMount *mt;
    RaceCarState *st;
    f32 x, y, z;
    int m, blocked, i, k;

    if (car == NULL)
        return 0;
    m = getMount();
    self = car;
    mt = &self->model.data->info->mounts[m];
    st = self->state;
    x = car_view_x(st, 0.0f);
    y = car_view_y(st, 0.0f);
    z = car_view_z(st, 0.0f);
    if (m < MOUNT_FIRST_CHECKED)
        return 0;

    int intr = intr_suspend();
    pgl_push_matrix();
    vu0_load_identity_regs();
    pgl_translate(mt->x, mt->y, mt->z);
    vu0_rotate_z(car_roll(st, 0.0f) * RAD_TO_DEG);
    vu0_rotate_x(-car_pitch(st, 0.0f) * RAD_TO_DEG);
    vu0_rotate_y(-car_yaw(st, 0.0f) * RAD_TO_DEG);
    pgl_translate(-x, -y, -z);
    vu0_invert_rigid();
    cam[2] = cam[1] = cam[0] = 0.0f;
    cam[3] = 1.0f;
    vu0_apply_matrix(cam, cam);
    pgl_pop_matrix();
    intr_resume(intr);

    sector = -1;
    p[0] = cam[0];
    p[1] = cam[1];
    p[2] = cam[2];
    org->course->getPlane(p, &sector, plane);
    blocked = 0;
    if (!(sector > -1))
        blocked = 1;

    int intr2 = intr_suspend();
    pgl_push_matrix();
    for (i = 0; i < org->entries->count; i++) {
        CarBox *box;
        f32 front, rear, halfWidth;

        if (i == carIndex)
            continue;
        box = &org->entries->cars[i]->state->dc->GetCollisionProcessor()->box[i];
        front = box->front + 0.04f;
        rear = box->rear + 0.04f;
        halfWidth = box->halfWidth + 0.05f;
        vu0_load_identity_regs();
        pgl_translate(box->statePos[0], box->statePos[2], -box->statePos[1]);
        vu0_rotate_y(box->stateYaw * RAD_TO_DEG_B);
        /* a point on each face, in the box's frame */
        facePoint[0][0] = halfWidth;
        facePoint[0][1] = OCCLUSION_BOX_HEIGHT;
        facePoint[0][3] = 1.0f;
        facePoint[0][2] = -front;
        facePoint[1][2] = -front;
        facePoint[1][0] = -halfWidth;
        facePoint[1][1] = OCCLUSION_BOX_HEIGHT;
        facePoint[1][3] = 1.0f;
        facePoint[2][0] = -halfWidth;
        facePoint[2][1] = OCCLUSION_BOX_HEIGHT;
        facePoint[2][2] = rear;
        facePoint[2][3] = 1.0f;
        facePoint[3][0] = halfWidth;
        facePoint[3][2] = rear;
        facePoint[3][1] = OCCLUSION_BOX_HEIGHT;
        facePoint[3][3] = 1.0f;
        facePoint[4][0] = 0.0f;
        facePoint[4][1] = OCCLUSION_BOX_HEIGHT;
        facePoint[4][2] = 0.0f;
        facePoint[4][3] = 1.0f;
        memset(facePoint[5], 0, 0x10);
        facePoint[5][3] = 1.0f;
        for (k = 0; k < 6; k++)
            vu0_apply_matrix(world[k], facePoint[k]);
        for (k = 0; k < 6; k++) {
            vu0_apply_matrix(planes[k], g_0033E1A8[k]);
            planes[k][3] = -dot3(planes[k], world[k]);
        }
        if (isInsidePlanes(planes, p) != 0)
            blocked = 1;
    }
    pgl_pop_matrix();
    intr_resume(intr2);
    return blocked;
}

/* Mount 0 only: 1 when the camera's position is inside other's box (other
 * within 5 m; not this camera's own car; the box OCCLUSION_BOX_HEIGHT up,
 * Lupo 2 L2 0x0020D010).  org is not used.
 * (89.9%, open: the facePoint store order, as isViewBlocked.) */
// FUNC 0x00200E08 OnBoardCamera::isInsideCar
int OnBoardCamera::isInsideCar(RaceOrganization *org, RaceEntryCar *other)
{
    f32 p[3];                               /* sp+0x00 */
    f32 planes[6][4];                       /* sp+0x10 */
    f32 facePoint[6][4];                    /* sp+0x70 */
    f32 world[6][4];                        /* sp+0xD0 */
    RaceCarState *st;
    CarBox *box;
    f32 dx, dy, dz;
    f32 front, rear, halfWidth;
    int intr, inside, k;

    if (mount != MOUNT_SWAY)
        return 0;
    if (car == NULL)
        return 0;
    if (other == NULL)
        return 0;
    if (other == car)
        return 0;
    st = other->state;
    p[0] = getPositionX();
    p[1] = getPositionY();
    p[2] = getPositionZ();
    dx = car_view_x(st, 0.0f) - p[0];
    dy = car_view_y(st, 0.0f) - p[1];
    dz = car_view_z(st, 0.0f) - p[2];
    if (dx * dx + dy * dy + dz * dz > 25.0f)
        return 0;

    intr = intr_suspend();
    pgl_push_matrix();
    box = &st->dc->GetCollisionProcessor()->box[other->number];
    front = box->front + 0.04f;
    rear = box->rear + 0.04f;
    halfWidth = box->halfWidth + 0.05f;
    vu0_load_identity_regs();
    pgl_translate(box->statePos[0], box->statePos[2], -box->statePos[1]);
    vu0_rotate_y(box->stateYaw * RAD_TO_DEG_B);
    facePoint[0][0] = halfWidth;
    facePoint[0][1] = OCCLUSION_BOX_HEIGHT;
    facePoint[0][3] = 1.0f;
    facePoint[0][2] = -front;
    facePoint[1][2] = -front;
    facePoint[1][0] = -halfWidth;
    facePoint[1][1] = OCCLUSION_BOX_HEIGHT;
    facePoint[1][3] = 1.0f;
    facePoint[2][0] = -halfWidth;
    facePoint[2][1] = OCCLUSION_BOX_HEIGHT;
    facePoint[2][2] = rear;
    facePoint[2][3] = 1.0f;
    facePoint[3][0] = halfWidth;
    facePoint[3][2] = rear;
    facePoint[3][1] = OCCLUSION_BOX_HEIGHT;
    facePoint[3][3] = 1.0f;
    facePoint[4][2] = 0.0f;
    facePoint[4][0] = 0.0f;
    facePoint[4][1] = OCCLUSION_BOX_HEIGHT;
    facePoint[4][3] = 1.0f;
    memset(facePoint[5], 0, 0x10);
    facePoint[5][3] = 1.0f;
    for (k = 0; k < 6; k++)
        vu0_apply_matrix(world[k], facePoint[k]);
    for (k = 0; k < 6; k++) {
        vu0_apply_matrix(planes[k], g_0033E208[k]);
        planes[k][3] = -dot3(planes[k], world[k]);
    }
    inside = isInsidePlanes(planes, p) != 0;
    pgl_pop_matrix();
    intr_resume(intr);
    return inside;
}

/* level is not used */
// FUNC 0x002010F8 OnBoardCamera::getChangeLevel
int OnBoardCamera::getChangeLevel(RaceOrganization &org, int level)
{
    return isViewBlocked(&org) ? 2 : 0;
}

// FUNC 0x00201120 OnBoardCamera::setMountChecked
void OnBoardCamera::setMountChecked(int m)
{
    mount = m;
    stepMount(0, 0);
}

// FUNC 0x00201148 isMountLocked
int isMountLocked(int m, int flags)
{
    return 0;
}

/* From the current mount + step (wrapping), the first mount with any of
 * `flags' (flags 0: any mount); mount 0 when none.  step 0 validates the
 * current one. */
// FUNC 0x00201150 OnBoardCamera::stepMount
void OnBoardCamera::stepMount(int step, int flags)
{
    CarModelInfo *info;
    int cur, count, m, tries;
    int first = (step != 0);                /* stepping skips the current mount */

    mountFlag2 = 0;
    cur = mount;
    mount = 0;
    info = car->model.data->info;
    count = info->mountCount;
    if (count <= 0)
        return;
    m = cur + step;
    for (tries = first; tries < count; tries++) {
        CameraMount *mt;

        if (!(m > -1))
            m = count - 1;
        if (!(m < count))
            m = 0;
        mt = &info->mounts[m];
        if (((mt->flags & flags) != 0 || flags == 0) && isMountLocked(m, flags) == 0) {
            mount = m;
            mountFlag2 = (mt->flags >> 1) & 1;  /* MOUNT_DRAW_INSIDE */
            break;
        }
        m += step;
    }
}

/* From `start' downwards (an index out of range: the last mount), the first
 * mount with any of anyFlags and all of allFlags; mount 0 when none. */
// FUNC 0x00201260 OnBoardCamera::findMountDown
void OnBoardCamera::findMountDown(int start, int anyFlags, int allFlags)
{
    CarModelInfo *info;
    int count, m, tries;

    mount = 0;
    mountFlag2 = 0;
    info = car->model.data->info;
    count = info->mountCount;
    if (count <= 0)
        return;
    m = start;
    for (tries = 0; tries < count; tries++, m--) {
        CameraMount *mt;

        if (m >= count || m < 0)
            m = count - 1;
        mt = &info->mounts[m];
        if ((mt->flags & anyFlags) == 0)
            continue;
        if ((mt->flags & allFlags) != allFlags)
            continue;
        if (isMountLocked(m, anyFlags) != 0)
            continue;
        mount = m;
        mountFlag2 = (mt->flags >> 1) & 1;
        return;
    }
}

// FUNC 0x00201368 OnBoardCamera::getFadeOutTime(void)
f32 OnBoardCamera::getFadeOutTime()
{
    return 0.0f;
}

// FUNC 0x00201378 OnBoardCamera::isDrawBackMirror(void)
int OnBoardCamera::isDrawBackMirror()
{
    return drawMirror;
}

// FUNC 0x00201380 OnBoardCamera::isEnableTargetChange(void)
int OnBoardCamera::isEnableTargetChange()
{
    return 1;
}

// FUNC 0x00201388 OnBoardCamera::isEnableChanging(void)
int OnBoardCamera::isEnableChanging()
{
    return 1;
}

// FUNC 0x00201390 OnBoardCamera::isNeedReset(RaceOrganization&, CameraTargetInfo&)
int OnBoardCamera::isNeedReset(RaceOrganization &org, CameraTargetInfo &target)
{
    return 0;
}

// FUNC 0x00201398 OnBoardCamera::isTargetDistEnable(void)
int OnBoardCamera::isTargetDistEnable()
{
    return 0;
}

// FUNC 0x002013A0 OnBoardCamera::getMount(void)
int OnBoardCamera::getMount()
{
    return mount;
}

/* no callers */
// FUNC 0x002013A8 OnBoardCamera::setMountChangeable
void OnBoardCamera::setMountChangeable(int on)
{
    changeable = on;
}

/* no callers */
// FUNC 0x002013B0 OnBoardCamera::isMountChangeable
int OnBoardCamera::isMountChangeable()
{
    return changeable;
}

/* no callers */
// FUNC 0x002013B8 OnBoardCamera::getCar
RaceEntryCar *OnBoardCamera::getCar()
{
    return car;
}

/* no callers */
// FUNC 0x002013C0 OnBoardCamera::setFov
void OnBoardCamera::setFov(f32 deg)
{
    fov = deg;
}

