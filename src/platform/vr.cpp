/* vr.cpp - PC VR (OpenXR): the headset session (vr.h, vr_view.h).  The big picture is in docs/vr.md.
 *
 * This file is the only one that talks to OpenXR.  In reading order:
 *   - session state and the [VR] settings (the globals just below),
 *   - swapchains: the textures the headset's compositor takes from us (Chain),
 *   - the head: a little quaternion maths, recentring, where panels hang (seat_panel),
 *   - the frame loop: next_frame (wait, begin, locate the eyes) and end_frame (submit the layers),
 *   - the overlay shader: draws the dashboard / mirror / menu pictures onto their panels,
 *   - the public API of vr.h, then the small adapter the game's own code sees (namespace vrview, at the end).
 *
 * Threads: everything runs on the game thread that presents, except overlay_draw, which the renderer
 * calls on its D3D thread (see vr.h). */
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include "platform/vr.h"
#include "platform/vr_view.h"
#include "platform/gpu.h"
#include "platform/input_map.h"
#include "platform/log.h"
#include "platform/paths.h"

#include <windows.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace vr {
namespace {

/* ---- the OpenXR session ---- */
XrInstance g_instance = XR_NULL_HANDLE;
XrSystemId g_system = XR_NULL_SYSTEM_ID;
XrSession g_session = XR_NULL_HANDLE;
XrSpace g_local = XR_NULL_HANDLE;       /* the world the compositor keeps still: where the player sat down */
XrSpace g_view = XR_NULL_HANDLE;        /* the head: moves with the headset */
ID3D11Device *g_device;                 /* the game's own device (the session is made on it) */
ID3D11DeviceContext *g_context;

/* ---- where the session is in its life ---- */
bool g_dead;                            /* failed or ended: no VR for the rest of the run */
bool g_running;                         /* xrBeginSession done, not ended */
bool g_frameOpen;                       /* xrBeginFrame done, xrEndFrame not yet: layers may be drawn */
bool g_shouldRender;                    /* the compositor wants pictures this frame (false: headset asleep) */
XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
XrTime g_displayTime;                   /* when this frame will be shown (what the head is located for) */
double g_hz;                            /* the headset's refresh rate, from the frame's predicted period */
bool g_recenter = true;                 /* place the flat screen and the seat at the next rendered frame */

/* ---- [VR] keys read once at start (vr.h documents them) ---- */
float g_menuDistance = 2.5f;            /* metres: where the flat screen hangs */
float g_menuWidth = 4.0f;               /* metres: its width */
float g_eyeScale = 1.0f;                /* the eye textures' size, times the runtime's recommended one */

/* ---- the settings the F2 menu changes ([VR], saved when changed) ----
 * Distances, heights and widths are in centimetres; WorldScale is a percentage.  Height 0 is at the
 * player's eyes, below is negative.  Each line: ini key, default, minimum, maximum, step. */
const SettingInfo kInfo[kSettingCount] = {
    /* kWorldScale      */ {"WorldScale", 100, 50, 200, 5},
    /* kHudDistance     */ {"HudDistance", 100, 40, 300, 5},
    /* kHudWidth        */ {"HudWidth", 70, 20, 200, 5},
    /* kHudHeight       */ {"HudHeight", -30, -100, 100, 5},
    /* kMirrorDistance  */ {"MirrorDistance", 90, 40, 300, 5},
    /* kMirrorWidth     */ {"MirrorWidth", 30, 10, 100, 5},
    /* kMirrorHeight    */ {"MirrorHeight", 20, -50, 80, 5},
    /* kScreenDistance  */ {"ScreenDistance", 90, 40, 300, 5},
    /* kScreenWidth     */ {"ScreenWidth", 80, 20, 250, 5},
    /* kScreenHeight    */ {"ScreenHeight", 0, -100, 100, 5},
};
int g_set[kSettingCount];
bool g_hudOn = true, g_mirrorOn = true;
bool g_loaded;

bool ini_on(const char *key, bool def)
{
    char on[16] = "";
    GetPrivateProfileStringA("VR", key, def ? "on" : "off", on, sizeof(on), paths::ini_path().c_str());
    return _stricmp(on, "on") == 0 || strcmp(on, "1") == 0;
}

void ensure_loaded()
{
    if (g_loaded)
        return;
    g_loaded = true;
    for (int i = 0; i < kSettingCount; i++) {
        const SettingInfo &s = kInfo[i];
        int v = (int)GetPrivateProfileIntA("VR", s.key, s.def, paths::ini_path().c_str());
        g_set[i] = v < s.min ? s.min : v > s.max ? s.max : v;
    }
    g_hudOn = ini_on("Hud", true);
    g_mirrorOn = ini_on("Mirror", true);
}

void save_value(const char *key, const char *value)
{
    WritePrivateProfileStringA("VR", key, value, paths::ini_path().c_str());
}

/* ---- the swapchains ------------------------------------------------------
 * A swapchain is a small ring of textures shared with the headset's compositor: we draw into the
 * image we acquire, release it at the end of the frame, and the compositor shows it.
 *
 * The runtime offers only sRGB 8-bit formats; the game's pictures are already gamma-encoded, so they
 * are drawn through a UNORM view of the same bytes (or, when the runtime's texture will not give one,
 * through a texture of ours copied over at the end of the frame). */
int64_t g_format;                       /* the swapchain format chosen (an sRGB one) */
DXGI_FORMAT g_unorm;                    /* the same bytes as plain UNORM: what we draw through */

struct Image {
    ID3D11Texture2D *tex = nullptr;     /* the compositor's texture */
    ID3D11RenderTargetView *rtv = nullptr;
};
struct Chain {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    int w = 0, h = 0;                   /* pixels */
    std::vector<Image> images;
    ID3D11Texture2D *shadow = nullptr;  /* ours, when a view of the images' can not be had */
    ID3D11RenderTargetView *shadowRtv = nullptr;
    bool acquired = false;              /* an image is out: drawn on this frame, not yet released */
    uint32_t index = 0;                 /* which of `images' it is */
};
Chain g_flat;                           /* the flat screen: a quad layer showing the game's ordinary picture */
Chain g_eye[2];                         /* the two eyes of the stereo projection layer */
constexpr int kFlatW = 2560, kFlatH = 1440;

/* The cockpit's panels (vr.h: kOverlayHud, kOverlayMirror, kOverlayScreen): a swapchain each, shown as a
 * quad layer.  Index = the overlay kind. */
Chain g_ov[kOverlayCount];
constexpr int kOvW[kOverlayCount] = {2048, 1280, 2048};    /* pixels: dashboard, mirror, menu */
constexpr int kOvH[kOverlayCount] = {1536, 384, 1536};
const char *const kOvName[kOverlayCount] = {"dashboard panel", "mirror panel", "menu panel"};
float g_ovAspect[kOverlayCount] = {4.0f / 3.0f, 3.75f, 4.0f / 3.0f};  /* width / height of the picture on the panel */

/* ---- the head ------------------------------------------------------------
 * OpenXR gives poses as a quaternion (rotation) and a vector (position); the little maths below is all
 * this file needs.  A quaternion (x, y, z, w) is a rotation: w is the cosine of half the angle. */
struct Quat {
    float x, y, z, w;
};
struct Vec3 {
    float x, y, z;
};
/* The product a * b: the rotation that does b first, then a. */
Quat quat_mul(Quat a, Quat b)
{
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
/* the opposite rotation (for a unit quaternion: its inverse) */
Quat quat_conjugate(Quat a) { return {-a.x, -a.y, -a.z, a.w}; }
/* the vector v turned by the rotation q */
Vec3 quat_rotate(Quat q, Vec3 v)
{
    /* v' = q v q*, v taken as the quaternion (v, 0) */
    const Quat p = quat_mul(quat_mul(q, {v.x, v.y, v.z, 0.0f}), quat_conjugate(q));
    return {p.x, p.y, p.z};
}
Quat to_quat(const XrQuaternionf &q) { return {q.x, q.y, q.z, q.w}; }
Vec3 to_vec3(const XrVector3f &v) { return {v.x, v.y, v.z}; }

/* Where the flat screen hangs in the local space (set by recenter: in front of the head, turned to it). */
XrPosef g_flatScreenPose = {{0, 0, 0, 1}, {0, 0, -2.5f}};
/* The seat: where the head was, and which way it faced (the yaw alone: the seat is level), at the last
 * recentre.  The car's frame is laid on it: "forward" is where the player looked then. */
Quat g_seatRotation = {0, 0, 0, 1};
Vec3 g_seatPosition = {0, 0, 0};

XrView g_views[2];                      /* this frame's two eyes (pose and field of view), in the local space */
bool g_viewsValid;                      /* g_views were located for this frame */
bool g_stereoFrame;                     /* the race frame being recorded is a stereo one (vrview::frame) */

float ini_float(const char *key, float def, float lo, float hi)
{
    char buf[32] = "";
    GetPrivateProfileStringA("VR", key, "", buf, sizeof(buf), paths::ini_path().c_str());
    if (!buf[0])
        return def;
    const float v = (float)atof(buf);
    return v < lo ? lo : v > hi ? hi : v;
}

/* Checks the result of an OpenXR call: true when it succeeded.  A failure is logged with the result's
 * name, and ends the session for good when it is the kind that does not come back.
 * `what' is the call's name, for the log. */
bool check_xr(XrResult result, const char *what)
{
    if (XR_SUCCEEDED(result))
        return true;
    char resultName[XR_MAX_RESULT_STRING_SIZE] = "?";
    if (g_instance)
        xrResultToString(g_instance, result, resultName);
    pclog::write("vr: %s failed: %s (%d)", what, resultName, (int)result);
    if (result == XR_ERROR_SESSION_LOST || result == XR_ERROR_INSTANCE_LOST || result == XR_ERROR_RUNTIME_FAILURE)
        g_dead = true;
    return false;
}

/* The session is over (the runtime stopped it, or went away): the game carries on, flat, without VR. */
void quit_session(const char *why)
{
    pclog::write("vr: session ended (%s): the game goes on without VR", why);
    g_dead = true;
    g_running = false;
    g_frameOpen = false;
}

XrPosef pose_identity()
{
    XrPosef p = {{0, 0, 0, 1}, {0, 0, 0}};
    return p;
}

constexpr float kDegreesPerRadian = 57.29578f;

/* Sets the seat and the flat screen from where the head is now: the seat is the head's position, level,
 * facing the way the head faces (yaw only); the flat screen hangs g_menuDistance in front of it.
 * Run at the first rendered frame, when the headset is put on again, and on the recentre key. */
void recenter(XrTime time)
{
    XrSpaceLocation head = {XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(g_view, g_local, time, &head)))
        return;
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if ((head.locationFlags & need) != need)
        return;
    const Quat q = to_quat(head.pose.orientation);
    /* the head's forward direction (-z) turned by q, then flattened onto the floor (y dropped) and made
     * a unit vector: (forwardX, forwardZ) */
    const float rawX = -2.0f * (q.x * q.z + q.w * q.y);
    const float rawZ = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    const float length = sqrtf(rawX * rawX + rawZ * rawZ);
    if (length < 0.01f)
        return;                         /* looking straight up or down: no heading to take, try again */
    const float forwardX = rawX / length, forwardZ = rawZ / length;
    const float yaw = atan2f(-forwardX, -forwardZ);     /* the heading as a turn about the up axis */
    const Quat yawOnly = {0.0f, sinf(yaw * 0.5f), 0.0f, cosf(yaw * 0.5f)};
    g_flatScreenPose.orientation = {yawOnly.x, yawOnly.y, yawOnly.z, yawOnly.w};
    g_flatScreenPose.position = {head.pose.position.x + forwardX * g_menuDistance, head.pose.position.y,
                                 head.pose.position.z + forwardZ * g_menuDistance};
    g_seatRotation = yawOnly;
    g_seatPosition = to_vec3(head.pose.position);
    g_recenter = false;
    pclog::write("vr: centred: the head at (%.2f %.2f %.2f), facing %.0f degrees", g_seatPosition.x,
                 g_seatPosition.y, g_seatPosition.z, yaw * kDegreesPerRadian);
}

void free_chain(Chain &c)
{
    for (auto &im : c.images)
        if (im.rtv)
            im.rtv->Release();
    c.images.clear();
    if (c.shadowRtv)
        c.shadowRtv->Release();
    if (c.shadow)
        c.shadow->Release();
    c.shadowRtv = nullptr;
    c.shadow = nullptr;
    if (c.swapchain)
        xrDestroySwapchain(c.swapchain);
    c.swapchain = XR_NULL_HANDLE;
    c.acquired = false;
}

/* Chooses the swapchain format: the first 8-bit sRGB one the runtime offers (RGBA, else BGRA), and the
 * UNORM twin of it that we draw through (g_unorm). */
bool pick_format()
{
    uint32_t count = 0;
    if (!check_xr(xrEnumerateSwapchainFormats(g_session, 0, &count, nullptr), "xrEnumerateSwapchainFormats"))
        return false;
    std::vector<int64_t> formats(count);
    if (!check_xr(xrEnumerateSwapchainFormats(g_session, count, &count, formats.data()),
                  "xrEnumerateSwapchainFormats"))
        return false;
    g_format = 0;
    for (int64_t format : formats)
        if (format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
            g_format = format;
    if (!g_format)
        for (int64_t format : formats)
            if (format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
                g_format = format;
    if (!g_format) {
        pclog::write("vr: no 8-bit sRGB swapchain format offered");
        return false;
    }
    g_unorm = g_format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
    return true;
}

/* Makes a swapchain of w x h pixels into `chain', with a render target for each of its images.
 * `what' names it in the log. */
bool make_chain(Chain &chain, int w, int h, const char *what)
{
    XrSwapchainCreateInfo swapchainInfo = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                               XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    swapchainInfo.format = g_format;
    swapchainInfo.sampleCount = 1;
    swapchainInfo.width = (uint32_t)w;
    swapchainInfo.height = (uint32_t)h;
    swapchainInfo.faceCount = 1;
    swapchainInfo.arraySize = 1;
    swapchainInfo.mipCount = 1;
    char label[64];
    snprintf(label, sizeof(label), "xrCreateSwapchain (%s)", what);
    if (!check_xr(xrCreateSwapchain(g_session, &swapchainInfo, &chain.swapchain), label)) {
        chain.swapchain = XR_NULL_HANDLE;
        return false;
    }
    chain.w = w;
    chain.h = h;
    uint32_t count = 0;
    if (!check_xr(xrEnumerateSwapchainImages(chain.swapchain, 0, &count, nullptr), "xrEnumerateSwapchainImages"))
        return false;
    std::vector<XrSwapchainImageD3D11KHR> runtimeImages(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (!check_xr(xrEnumerateSwapchainImages(chain.swapchain, count, &count,
                                             (XrSwapchainImageBaseHeader *)runtimeImages.data()),
                  "xrEnumerateSwapchainImages"))
        return false;
    /* a UNORM render target view on each of the runtime's (sRGB) textures */
    bool viewsMade = true;
    for (auto &runtimeImage : runtimeImages) {
        Image image;
        image.tex = runtimeImage.texture;
        D3D11_RENDER_TARGET_VIEW_DESC viewDesc = {};
        viewDesc.Format = g_unorm;
        viewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(g_device->CreateRenderTargetView(image.tex, &viewDesc, &image.rtv))) {
            viewsMade = false;
            break;
        }
        chain.images.push_back(image);
    }
    if (!viewsMade) {
        /* the runtime's texture gives no UNORM view: draw into a texture of ours, copied into the
         * swapchain's image at the end of the frame (release) */
        for (auto &image : chain.images)
            if (image.rtv)
                image.rtv->Release();
        chain.images.clear();
        for (auto &runtimeImage : runtimeImages) {
            Image image;
            image.tex = runtimeImage.texture;
            chain.images.push_back(image);
        }
        D3D11_TEXTURE2D_DESC textureDesc = {};
        textureDesc.Width = (UINT)w;
        textureDesc.Height = (UINT)h;
        textureDesc.MipLevels = textureDesc.ArraySize = 1;
        textureDesc.Format = g_unorm;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(g_device->CreateTexture2D(&textureDesc, nullptr, &chain.shadow)) ||
            FAILED(g_device->CreateRenderTargetView(chain.shadow, nullptr, &chain.shadowRtv))) {
            pclog::write("vr: the %s's render target could not be made", what);
            return false;
        }
        pclog::write("vr: the %s is drawn through a copy (the runtime's texture has no UNORM view)", what);
    }
    pclog::write("vr: %s %dx%d, %u images, format %d", what, w, h, count, (int)g_format);
    return true;
}

/* Takes the next image of the swapchain to draw on: `*rtv' is its render target.  false: none (the
 * swapchain is missing, or an image is already out, or the runtime failed). */
bool acquire(Chain &chain, ID3D11RenderTargetView **rtv)
{
    if (chain.swapchain == XR_NULL_HANDLE || chain.acquired)
        return false;
    XrSwapchainImageAcquireInfo acquireInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!check_xr(xrAcquireSwapchainImage(chain.swapchain, &acquireInfo, &chain.index), "xrAcquireSwapchainImage"))
        return false;
    XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = XR_INFINITE_DURATION;
    if (!check_xr(xrWaitSwapchainImage(chain.swapchain, &waitInfo), "xrWaitSwapchainImage")) {
        XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(chain.swapchain, &releaseInfo);
        return false;
    }
    chain.acquired = true;
    *rtv = chain.shadowRtv ? chain.shadowRtv : chain.images[chain.index].rtv;
    return true;
}

/* Hands the image drawn on back to the runtime (its commands have been given to the device: the D3D
 * thread was idle).  With a shadow texture, it is copied into the swapchain's image first. */
void release(Chain &chain)
{
    if (!chain.acquired)
        return;
    if (chain.shadow)
        g_context->CopyResource(chain.images[chain.index].tex, chain.shadow);
    XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    check_xr(xrReleaseSwapchainImage(chain.swapchain, &releaseInfo), "xrReleaseSwapchainImage");
    chain.acquired = false;
}

/* Opens the next frame: xrWaitFrame (which blocks until the headset wants it: this is what paces the
 * game to the headset's rate) and xrBeginFrame, then, when the frame is to be drawn, recentres if asked
 * and locates the two eyes where they will be when the frame is shown. */
void next_frame()
{
    g_viewsValid = false;
    XrFrameState frameState = {XR_TYPE_FRAME_STATE};
    if (!check_xr(xrWaitFrame(g_session, nullptr, &frameState), "xrWaitFrame"))
        return;
    if (!check_xr(xrBeginFrame(g_session, nullptr), "xrBeginFrame"))
        return;
    g_frameOpen = true;
    g_shouldRender = frameState.shouldRender != 0;
    g_displayTime = frameState.predictedDisplayTime;
    if (frameState.predictedDisplayPeriod > 0)
        g_hz = 1.0e9 / (double)frameState.predictedDisplayPeriod;    /* (the period is in nanoseconds) */
    if (!g_shouldRender)
        return;
    if (g_recenter)
        recenter(g_displayTime);
    XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locateInfo.displayTime = g_displayTime;
    locateInfo.space = g_local;
    XrViewState viewState = {XR_TYPE_VIEW_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t viewCount = 0;
    if (XR_FAILED(xrLocateViews(g_session, &locateInfo, &viewState, 2, &viewCount, views)) || viewCount != 2)
        return;
    const XrViewStateFlags need = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    if ((viewState.viewStateFlags & need) != need)
        return;
    g_views[0] = views[0];
    g_views[1] = views[1];
    g_viewsValid = true;
}

/* The runtime's events, once a frame: the session's life cycle (begin it when READY, end it when told),
 * and anything that makes the seat stale (the headset put on again, the runtime moving its space). */
void poll_events()
{
    for (;;) {
        XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
        if (xrPollEvent(g_instance, &event) != XR_SUCCESS)
            return;
        switch (event.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            const auto *change = (const XrEventDataSessionStateChanged *)&event;
            const XrSessionState before = g_state;
            g_state = change->state;
            pclog::write("vr: session state %d", (int)change->state);
            /* the headset put on again (SteamVR sleeps the screens off the head: the
             * session falls back to synchronized): the flat screen and the seat are placed anew */
            if (before == XR_SESSION_STATE_SYNCHRONIZED && change->state == XR_SESSION_STATE_VISIBLE)
                g_recenter = true;
            if (change->state == XR_SESSION_STATE_READY && !g_running) {
                XrSessionBeginInfo beginInfo = {XR_TYPE_SESSION_BEGIN_INFO};
                beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (check_xr(xrBeginSession(g_session, &beginInfo), "xrBeginSession")) {
                    g_running = true;
                    next_frame();
                }
            } else if (change->state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(g_session);
                quit_session("the runtime stopped it");
            } else if (change->state == XR_SESSION_STATE_EXITING || change->state == XR_SESSION_STATE_LOSS_PENDING) {
                quit_session("the runtime exited it");
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
            g_recenter = true;
            break;
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            quit_session("the runtime is going away");
            break;
        default:
            break;
        }
    }
}

bool our_window_has_focus()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

/* The VR hotkeys, each with the action it does and whether holding the key repeats it.  They are the
 * hotkeys input_map.h declares (rebindable in the launcher); window.cpp handles them while the game's
 * window has the keyboard. */
struct VrHotkey {
    int hotkey;                         /* input_map::kHotVr* */
    Action action;
    bool repeatsWhenHeld;               /* the world's scale keys: held, they keep stepping */
};
constexpr VrHotkey kVrHotkeys[] = {
    {input_map::kHotVrRecenter, kActRecenter, false},
    {input_map::kHotVrHud, kActHud, false},
    {input_map::kHotVrMirror, kActMirror, false},
    {input_map::kHotVrScaleDown, kActScaleDown, true},
    {input_map::kHotVrScaleUp, kActScaleUp, true},
};
constexpr int kVrHotkeyCount = (int)(sizeof(kVrHotkeys) / sizeof(kVrHotkeys[0]));
constexpr ULONGLONG kRepeatDelayMs = 400;       /* a held scale key: the wait before it starts repeating ... */
constexpr ULONGLONG kRepeatIntervalMs = 90;     /* ... and the time between repeats */

/* The VR hotkeys while the game's window has not the keyboard: the player is in the headset, so the keys
 * are read from the system instead (called once a frame by end_frame). */
void poll_hotkeys()
{
    static bool wasDown[kVrHotkeyCount];
    static ULONGLONG nextRepeatAt[kVrHotkeyCount];
    const ULONGLONG now = GetTickCount64();
    const bool ctrl = GetAsyncKeyState(VK_CONTROL) < 0, alt = GetAsyncKeyState(VK_MENU) < 0,
               shift = GetAsyncKeyState(VK_SHIFT) < 0;
    for (int i = 0; i < kVrHotkeyCount; i++) {
        const VrHotkey &entry = kVrHotkeys[i];
        const input_map::KeyCombo &keys = input_map::hotkey(entry.hotkey);
        /* pressed: the key is down and the modifiers are exactly the ones bound */
        const bool pressed = keys.vk != 0 && (GetAsyncKeyState(keys.vk) & 0x8000) != 0 && keys.ctrl == ctrl &&
                             keys.alt == alt && keys.shift == shift;
        const bool repeatDue = entry.repeatsWhenHeld && now >= nextRepeatAt[i];
        if (pressed && (!wasDown[i] || repeatDue)) {
            nextRepeatAt[i] = now + (wasDown[i] ? kRepeatIntervalMs : kRepeatDelayMs);
            hotkey(entry.action);
        }
        wasDown[i] = pressed;
    }
}

/* ---- the overlays' own drawing --------------------------------------------
 * The game draws the dashboard, the mirror and the menu onto a picture of its own (the "overlay
 * picture", see docs/vr.md); one pass here copies the part of it that belongs to a panel onto that
 * panel's swapchain image.  The vertex shader draws one triangle that covers the whole target; the
 * pixel shader copies, and for the keyed panels (dashboard, menu) turns black into clear. */
struct OverlayParams {                  /* the shader's constant buffer (cbuffer OverlayParams below) */
    float uvRect[4];                    /* the part of the picture to copy: u0, v0, u1, v1 */
    float keyed;                        /* 1: black is clear; 0: opaque */
    float keyGain;                      /* keyed: how fast brighter means more opaque */
    float unused[2];                    /* (pads it to 32 bytes: a constant buffer's size is a multiple of 16) */
};
static_assert(sizeof(OverlayParams) == 32, "the HLSL cbuffer below has 8 floats");
constexpr float kKeyGain = 4.0f;        /* alpha = brightest colour channel x 4, held to 1: dim parts stay readable */

const char kOverlayHlsl[] = R"HLSL(
cbuffer OverlayParams : register(b0)
{
    float4 uvRect;      // u0, v0, u1, v1: the part of the picture shown over the whole target
    float  keyed;       // 1: black is clear (the dashboard, the menu); 0: opaque (the mirror)
    float  keyGain;     // keyed: alpha = the brightest channel x keyGain, held to 0..1
    float2 unused;
};
struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
// One triangle that covers the target: vertices 0, 1, 2 -> corners (0,0), (2,0), (0,2) of a 0..1 square.
VO vs_ov(uint id : SV_VertexID)
{
    float2 corner = float2((id << 1) & 2, id & 2);
    VO o;
    o.pos = float4(corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0, 0.0, 1.0);
    o.uv = lerp(uvRect.xy, uvRect.zw, corner);
    return o;
}
Texture2D tex : register(t0);
SamplerState smp : register(s0);
float4 ps_ov(VO i) : SV_Target
{
    float4 color = tex.Sample(smp, i.uv);
    float alpha = keyed > 0.5 ? saturate(max(color.r, max(color.g, color.b)) * keyGain) : 1.0;
    return float4(color.rgb, alpha);
}
)HLSL";

struct OverlayGfx {
    ID3D11VertexShader *vs = nullptr;
    ID3D11PixelShader *ps = nullptr;
    ID3D11SamplerState *sampler = nullptr;
    ID3D11Buffer *cb = nullptr;
    ID3D11RasterizerState *raster = nullptr;
    bool tried = false;
} g_ovGfx;

bool overlay_gfx()
{
    if (g_ovGfx.tried)
        return g_ovGfx.ps != nullptr;
    g_ovGfx.tried = true;
    ID3DBlob *code = nullptr;
    g_ovGfx.vs = gpu::compile_vs("vr overlay", kOverlayHlsl, "vs_ov", &code);
    if (code)
        code->Release();
    g_ovGfx.ps = gpu::compile_ps("vr overlay", kOverlayHlsl, "ps_ov");
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    g_device->CreateSamplerState(&sd, &g_ovGfx.sampler);
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(OverlayParams);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_device->CreateBuffer(&bd, nullptr, &g_ovGfx.cb);
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    g_device->CreateRasterizerState(&rd, &g_ovGfx.raster);
    return g_ovGfx.ps != nullptr && g_ovGfx.vs != nullptr && g_ovGfx.sampler && g_ovGfx.cb && g_ovGfx.raster;
}

/* Where a panel of the cockpit hangs: `distance' metres in front of the seat (the way the player faced at
 * the last recentre), `height' metres above the eyes (below is negative), its face tilted to look back at
 * the seat.  Panels are world-locked: they stay where they are when the player moves their head. */
XrPosef seat_panel(float distance, float height)
{
    const Quat seatYaw = g_seatRotation;
    const Vec3 offset = quat_rotate(seatYaw, {0.0f, height, -distance});     /* -z is forward */
    const float tilt = atan2f(height, distance);        /* a panel above the eyes faces down, below faces up */
    const Quat tiltAboutX = {sinf(tilt * 0.5f), 0.0f, 0.0f, cosf(tilt * 0.5f)};
    const Quat orientation = quat_mul(seatYaw, tiltAboutX);
    XrPosef pose;
    pose.orientation = {orientation.x, orientation.y, orientation.z, orientation.w};
    pose.position = {g_seatPosition.x + offset.x, g_seatPosition.y + offset.y, g_seatPosition.z + offset.z};
    return pose;
}

} // namespace

bool wanted()
{
    char on[16] = "";
    GetPrivateProfileStringA("VR", "Enabled", "off", on, sizeof(on), paths::ini_path().c_str());
    return _stricmp(on, "on") == 0 || strcmp(on, "1") == 0;
}

/* Step one of starting VR, before the game's Direct3D device exists: is it wanted, is there a runtime and
 * a headset, and which graphics adapter does the runtime want the device made on (`*adapter')?
 * false: no VR this run; the reason is in the log. */
bool prepare(LUID *adapter)
{
    if (!wanted())
        return false;
    /* the loader is delay-loaded (cmake/openxr.cmake): without its DLL the game must still start */
    if (LoadLibraryA("openxr_loader.dll") == nullptr) {
        pclog::write("vr: openxr_loader.dll was not found next to the game: no VR");
        return false;
    }
    /* the runtime must support Direct3D 11, the game's own API */
    uint32_t extensionCount = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr);
    std::vector<XrExtensionProperties> extensions(extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data());
    bool hasD3D11 = false;
    for (auto &extension : extensions)
        if (strcmp(extension.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0)
            hasD3D11 = true;
    if (!hasD3D11) {
        pclog::write("vr: the OpenXR runtime has no Direct3D 11 support: no VR");
        return false;
    }
    const char *wantedExtensions[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo instanceInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(instanceInfo.applicationInfo.applicationName, "Gran Turismo 3 PC");
    instanceInfo.applicationInfo.applicationVersion = 1;
    strcpy(instanceInfo.applicationInfo.engineName, "GT3PC");
    instanceInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    instanceInfo.enabledExtensionCount = 1;
    instanceInfo.enabledExtensionNames = wantedExtensions;
    XrResult result = xrCreateInstance(&instanceInfo, &g_instance);
    if (XR_FAILED(result)) {
        g_instance = XR_NULL_HANDLE;
        pclog::write("vr: no OpenXR runtime could be started (%d): no VR", (int)result);
        return false;
    }
    XrInstanceProperties instanceProps = {XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(g_instance, &instanceProps);
    pclog::write("vr: runtime %s %u.%u.%u", instanceProps.runtimeName, XR_VERSION_MAJOR(instanceProps.runtimeVersion),
                 XR_VERSION_MINOR(instanceProps.runtimeVersion), XR_VERSION_PATCH(instanceProps.runtimeVersion));
    XrSystemGetInfo systemInfo = {XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    result = xrGetSystem(g_instance, &systemInfo, &g_system);
    if (XR_FAILED(result)) {
        pclog::write("vr: no headset found (%d; is it on, with SteamVR running?): no VR", (int)result);
        xrDestroyInstance(g_instance);
        g_instance = XR_NULL_HANDLE;
        return false;
    }
    XrSystemProperties systemProps = {XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(g_instance, g_system, &systemProps);
    pclog::write("vr: headset system \"%s\"", systemProps.systemName);
    /* the adapter the runtime renders on (the device must be made on it: gpu.cpp) */
    PFN_xrGetD3D11GraphicsRequirementsKHR getRequirements = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(g_instance, "xrGetD3D11GraphicsRequirementsKHR",
                                        (PFN_xrVoidFunction *)&getRequirements)) ||
        getRequirements == nullptr) {
        pclog::write("vr: xrGetD3D11GraphicsRequirementsKHR is missing: no VR");
        return false;
    }
    XrGraphicsRequirementsD3D11KHR requirements = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (!check_xr(getRequirements(g_instance, g_system, &requirements), "xrGetD3D11GraphicsRequirementsKHR"))
        return false;
    *adapter = requirements.adapterLuid;
    ensure_loaded();
    g_menuDistance = ini_float("MenuDistance", 2.5f, 0.8f, 10.0f);
    g_menuWidth = ini_float("MenuWidth", 4.0f, 0.5f, 20.0f);
    g_eyeScale = ini_float("RenderScale", 1.0f, 0.5f, 2.0f);
    /* MenuFov: the flat screen's width as the angle it covers (THX recommends about 40 degrees, SMPTE 30);
     * the width follows from the distance: width = 2 x distance x tan(angle / 2) */
    const float menuFovDegrees = ini_float("MenuFov", 0.0f, 0.0f, 120.0f);
    constexpr float kMinMenuFovDegrees = 5.0f;          /* (below it: off, the width is MenuWidth) */
    constexpr float kRadiansPerDegree = 3.14159265f / 180.0f;
    if (menuFovDegrees >= kMinMenuFovDegrees)
        g_menuWidth = 2.0f * g_menuDistance * tanf(menuFovDegrees * 0.5f * kRadiansPerDegree);
    return true;
}

/* Step two, once the game's device exists: the session on it, the reference spaces, the swapchains.
 * false: no VR this run. */
bool start(ID3D11Device *device)
{
    if (g_instance == XR_NULL_HANDLE)
        return false;
    g_device = device;
    g_device->GetImmediateContext(&g_context);
    XrGraphicsBindingD3D11KHR graphicsBinding = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    graphicsBinding.device = device;
    XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &graphicsBinding;
    sessionInfo.systemId = g_system;
    if (!check_xr(xrCreateSession(g_instance, &sessionInfo, &g_session), "xrCreateSession")) {
        g_session = XR_NULL_HANDLE;
        g_dead = true;
        return false;
    }
    /* two reference spaces: LOCAL (still: where the player sat down) and VIEW (the head) */
    XrReferenceSpaceCreateInfo spaceInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.poseInReferenceSpace = pose_identity();
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    bool good = check_xr(xrCreateReferenceSpace(g_session, &spaceInfo, &g_local), "xrCreateReferenceSpace (local)");
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    good = good && check_xr(xrCreateReferenceSpace(g_session, &spaceInfo, &g_view), "xrCreateReferenceSpace (view)");
    if (!good || !pick_format() || !make_chain(g_flat, kFlatW, kFlatH, "flat screen")) {
        shutdown();
        g_dead = true;
        return false;
    }
    /* the eyes: the runtime's recommended size, scaled ([VR] RenderScale) */
    uint32_t viewCount = 0;
    std::vector<XrViewConfigurationView> viewConfigs;
    if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                       0, &viewCount, nullptr)) &&
        viewCount == 2) {
        viewConfigs.assign(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
        xrEnumerateViewConfigurationViews(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount,
                                          &viewCount, viewConfigs.data());
        const int w = std::min((int)viewConfigs[0].maxImageRectWidth,
                               std::max(256, (int)((float)viewConfigs[0].recommendedImageRectWidth * g_eyeScale + 0.5f)));
        const int h = std::min((int)viewConfigs[0].maxImageRectHeight,
                               std::max(256, (int)((float)viewConfigs[0].recommendedImageRectHeight * g_eyeScale + 0.5f)));
        if (!make_chain(g_eye[0], w, h, "left eye") || !make_chain(g_eye[1], w, h, "right eye")) {
            free_chain(g_eye[0]);
            free_chain(g_eye[1]);
            pclog::write("vr: no eye swapchains: the game stays on the flat screen");
        }
    }
    /* the dashboard's, the mirror's and the menu's panels */
    for (int k = 0; k < kOverlayCount; k++)
        if (!make_chain(g_ov[k], kOvW[k], kOvH[k], kOvName[k])) {
            free_chain(g_ov[k]);
            pclog::write("vr: no %s swapchain: it is not shown", kOvName[k]);
        }
    pclog::write("vr: session created; waiting for the headset to be ready");
    return true;
}

bool active()
{
    return g_session != XR_NULL_HANDLE && g_running && !g_dead;
}

void shutdown()
{
    if (g_session != XR_NULL_HANDLE) {
        if (g_running) {
            xrRequestExitSession(g_session);
            g_running = false;
        }
        free_chain(g_flat);
        free_chain(g_eye[0]);
        free_chain(g_eye[1]);
        for (int k = 0; k < kOverlayCount; k++)
            free_chain(g_ov[k]);
        if (g_view)
            xrDestroySpace(g_view);
        if (g_local)
            xrDestroySpace(g_local);
        g_view = g_local = XR_NULL_HANDLE;
        xrDestroySession(g_session);
        g_session = XR_NULL_HANDLE;
    }
    if (g_instance != XR_NULL_HANDLE) {
        xrDestroyInstance(g_instance);
        g_instance = XR_NULL_HANDLE;
    }
    if (g_context) {
        g_context->Release();
        g_context = nullptr;
    }
    g_dead = true;
}

bool flat_acquire(ID3D11RenderTargetView **rtv, int *width, int *height)
{
    /* a stereo frame has its projection layer instead */
    if (!active() || !g_frameOpen || !g_shouldRender || g_eye[0].acquired || g_eye[1].acquired)
        return false;
    if (!acquire(g_flat, rtv))
        return false;
    *width = g_flat.w;
    *height = g_flat.h;
    return true;
}

bool stereo_ready()
{
    return active() && g_frameOpen && g_shouldRender && g_viewsValid && g_eye[0].swapchain != XR_NULL_HANDLE &&
           g_eye[1].swapchain != XR_NULL_HANDLE;
}

void eye_size(int *width, int *height)
{
    *width = g_eye[0].w;
    *height = g_eye[0].h;
}

bool eye_acquire(int eye, ID3D11RenderTargetView **rtv, int *width, int *height)
{
    if ((eye != 0 && eye != 1) || !active() || !g_frameOpen || !g_shouldRender)
        return false;
    if (!acquire(g_eye[eye], rtv))
        return false;
    *width = g_eye[eye].w;
    *height = g_eye[eye].h;
    return true;
}

/* Ends the frame next_frame opened: hands the compositor the layers this frame drew, then opens the next
 * frame (which blocks until the headset wants it).  Which layers there are tells what the game drew:
 *   a stereo race frame:  the eyes (a projection layer), then the panels that were drawn (mirror,
 *                         dashboard, menu - in that order, so later ones are in front);
 *   anything else:        the flat screen (a quad layer) alone. */
void end_frame()
{
    if (g_session == XR_NULL_HANDLE || g_dead)
        return;
    poll_events();
    if (!our_window_has_focus())
        poll_hotkeys();                 /* (the window's own keys act while it has the keyboard) */
    if (!g_running || g_dead)
        return;
    if (g_frameOpen) {
        g_context->Flush();             /* the pictures' draws are given to the device before the runtime reads them */
        XrCompositionLayerProjectionView eyeViews[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        XrCompositionLayerProjection eyesLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        XrCompositionLayerQuad flatLayer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        XrCompositionLayerQuad panelLayers[kOverlayCount] = {{XR_TYPE_COMPOSITION_LAYER_QUAD},
                                                             {XR_TYPE_COMPOSITION_LAYER_QUAD},
                                                             {XR_TYPE_COMPOSITION_LAYER_QUAD}};
        const XrCompositionLayerBaseHeader *layers[1 + kOverlayCount + 1] = {};   /* eyes, panels, flat screen */
        uint32_t layerCount = 0;
        const bool eyesDrawn = g_eye[0].acquired && g_eye[1].acquired && g_viewsValid;
        for (int eye = 0; eye < 2; eye++)
            release(g_eye[eye]);
        if (eyesDrawn) {
            for (int eye = 0; eye < 2; eye++) {
                eyeViews[eye].pose = g_views[eye].pose;        /* the pose and field of view the eye was drawn for */
                eyeViews[eye].fov = g_views[eye].fov;
                eyeViews[eye].subImage.swapchain = g_eye[eye].swapchain;
                eyeViews[eye].subImage.imageRect = {{0, 0}, {g_eye[eye].w, g_eye[eye].h}};
            }
            eyesLayer.layerFlags = 0;
            eyesLayer.space = g_local;
            eyesLayer.viewCount = 2;
            eyesLayer.views = eyeViews;
            layers[layerCount++] = (const XrCompositionLayerBaseHeader *)&eyesLayer;
        }
        /* The panels of the cockpit hang where the seat is.  Back to front: the mirror, the dashboard over
         * it, the menu over both.  Each overlay kind's placement settings are in the tables below. */
        static const int kBackToFront[kOverlayCount] = {kOverlayMirror, kOverlayHud, kOverlayScreen};
        static const Setting kDistanceSetting[kOverlayCount] = {kHudDistance, kMirrorDistance, kScreenDistance};
        static const Setting kWidthSetting[kOverlayCount] = {kHudWidth, kMirrorWidth, kScreenWidth};
        static const Setting kHeightSetting[kOverlayCount] = {kHudHeight, kMirrorHeight, kScreenHeight};
        for (int order = 0; order < kOverlayCount; order++) {
            const int kind = kBackToFront[order];
            if (!g_ov[kind].acquired)
                continue;               /* the game drew nothing on this panel this frame */
            release(g_ov[kind]);
            const float distance = (float)setting(kDistanceSetting[kind]) / 100.0f;     /* (settings are in cm) */
            const float width = (float)setting(kWidthSetting[kind]) / 100.0f;
            const float height = (float)setting(kHeightSetting[kind]) / 100.0f;
            XrCompositionLayerQuad &panel = panelLayers[order];
            /* the dashboard and the menu have transparent parts (black is keyed out: the overlay shader) */
            panel.layerFlags = kind != kOverlayMirror ? (XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                                         XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT)
                                                      : 0;
            panel.space = g_local;
            panel.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            panel.subImage.swapchain = g_ov[kind].swapchain;
            panel.subImage.imageRect = {{0, 0}, {g_ov[kind].w, g_ov[kind].h}};
            panel.pose = seat_panel(distance, height);
            panel.size = {width, width / g_ovAspect[kind]};
            if (eyesDrawn)              /* (panels are part of the cockpit: without the eyes they are not shown) */
                layers[layerCount++] = (const XrCompositionLayerBaseHeader *)&panel;
        }
        if (g_flat.acquired) {
            release(g_flat);
            flatLayer.layerFlags = 0;
            flatLayer.space = g_local;
            flatLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            flatLayer.subImage.swapchain = g_flat.swapchain;
            flatLayer.subImage.imageRect = {{0, 0}, {g_flat.w, g_flat.h}};
            flatLayer.pose = g_flatScreenPose;
            flatLayer.size = {g_menuWidth, g_menuWidth * (float)g_flat.h / (float)g_flat.w};
            layers[layerCount++] = (const XrCompositionLayerBaseHeader *)&flatLayer;
        }
        XrFrameEndInfo frameEnd = {XR_TYPE_FRAME_END_INFO};
        frameEnd.displayTime = g_displayTime;
        frameEnd.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        frameEnd.layerCount = layerCount;
        frameEnd.layers = layers;
        check_xr(xrEndFrame(g_session, &frameEnd), "xrEndFrame");
        g_frameOpen = false;
    }
    g_stereoFrame = false;
    if (!g_dead)
        next_frame();
}

double refresh_hz()
{
    return active() ? g_hz : 0.0;
}

const SettingInfo &setting_info(Setting s)
{
    return kInfo[s];
}

int setting(Setting s)
{
    ensure_loaded();
    return g_set[s];
}

void set_setting(Setting s, int value)
{
    ensure_loaded();
    const SettingInfo &i = kInfo[s];
    value = value < i.min ? i.min : value > i.max ? i.max : value;
    if (value == g_set[s])
        return;
    g_set[s] = value;
    char b[16];
    snprintf(b, sizeof(b), "%d", value);
    save_value(i.key, b);
}

bool hud_on()
{
    ensure_loaded();
    return g_hudOn;
}

void set_hud_on(bool on)
{
    ensure_loaded();
    g_hudOn = on;
    save_value("Hud", on ? "on" : "off");
}

bool mirror_on()
{
    ensure_loaded();
    return g_mirrorOn;
}

void set_mirror_on(bool on)
{
    ensure_loaded();
    g_mirrorOn = on;
    save_value("Mirror", on ? "on" : "off");
}

bool available()
{
    return g_session != XR_NULL_HANDLE && !g_dead;
}

void hotkey(Action a)
{
    switch (a) {
    case kActRecenter:
        g_recenter = true;
        break;
    case kActHud:
        set_hud_on(!hud_on());
        pclog::write("vr: dashboard %s", hud_on() ? "on" : "off");
        break;
    case kActMirror:
        set_mirror_on(!mirror_on());
        pclog::write("vr: mirror %s", mirror_on() ? "on" : "off");
        break;
    case kActScaleDown:
    case kActScaleUp:
        set_setting(kWorldScale, setting(kWorldScale) + (a == kActScaleUp ? 1 : -1) * kInfo[kWorldScale].step);
        pclog::write("vr: world scale %d%%", setting(kWorldScale));
        break;
    }
}

bool overlay_wanted(int kind)
{
    if (kind < 0 || kind >= kOverlayCount)
        return false;
    /* (the menu panel has no switch: the game asks for it only while a menu or a result is up) */
    return (kind == kOverlayHud ? hud_on() : kind == kOverlayMirror ? mirror_on() : true) &&
           g_ov[kind].swapchain != XR_NULL_HANDLE;
}

bool overlay_acquire(int kind, float aspect, ID3D11RenderTargetView **rtv, int *width, int *height)
{
    if (!overlay_wanted(kind) || !active() || !g_frameOpen || !g_shouldRender)
        return false;
    if (!acquire(g_ov[kind], rtv))
        return false;
    if (aspect > 0.05f)
        g_ovAspect[kind] = aspect;
    *width = g_ov[kind].w;
    *height = g_ov[kind].h;
    return true;
}

void overlay_draw(ID3D11DeviceContext *dc, ID3D11ShaderResourceView *src, ID3D11RenderTargetView *rtv, int width,
                  int height, const float uv[4], int kind)
{
    if (!overlay_gfx())
        return;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(dc->Map(g_ovGfx.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return;
    OverlayParams *params = (OverlayParams *)m.pData;
    for (int i = 0; i < 4; i++)
        params->uvRect[i] = uv[i];
    params->keyed = kind != kOverlayMirror ? 1.0f : 0.0f;       /* the dashboard and the menu keyed, the mirror opaque */
    params->keyGain = kKeyGain;
    params->unused[0] = params->unused[1] = 0.0f;
    dc->Unmap(g_ovGfx.cb, 0);
    ID3D11ShaderResourceView *none = nullptr;
    const D3D11_VIEWPORT vp = {0, 0, (float)width, (float)height, 0, 1};
    dc->OMSetRenderTargets(1, &rtv, nullptr);
    dc->RSSetViewports(1, &vp);
    dc->RSSetState(g_ovGfx.raster);
    dc->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    dc->IASetInputLayout(nullptr);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->VSSetShader(g_ovGfx.vs, nullptr, 0);
    dc->VSSetConstantBuffers(0, 1, &g_ovGfx.cb);
    dc->PSSetShader(g_ovGfx.ps, nullptr, 0);
    dc->PSSetConstantBuffers(0, 1, &g_ovGfx.cb);
    dc->PSSetShaderResources(0, 1, &src);
    dc->PSSetSamplers(0, 1, &g_ovGfx.sampler);
    dc->Draw(3, 0);
    dc->PSSetShaderResources(0, 1, &none);
}

} // namespace vr

namespace vrview {

bool ready()
{
    return vr::stereo_ready();
}

void frame(bool on)
{
    vr::g_stereoFrame = on;
}

bool frame_on()
{
    return vr::g_stereoFrame;
}

bool hud_on()
{
    return vr::hud_on();
}

bool mirror_on()
{
    return vr::mirror_on();
}

void eye_size(int *width, int *height)
{
    vr::eye_size(width, height);
}

bool eye(int eyeIndex, float tan[4], float viewInv[16])
{
    if ((eyeIndex != 0 && eyeIndex != 1) || !vr::g_viewsValid)
        return false;
    const XrView &view = vr::g_views[eyeIndex];
    /* the field of view: the four half-angles as tangents (what a projection matrix is built from) */
    tan[0] = tanf(view.fov.angleLeft);
    tan[1] = tanf(view.fov.angleRight);
    tan[2] = tanf(view.fov.angleDown);
    tan[3] = tanf(view.fov.angleUp);
    /* The eye's pose in the seat's frame: where the eye is and how it is turned, measured from the seat
     * (the head as it was at the last recentre, facing forward) instead of from the runtime's own space. */
    const vr::Quat toSeat = vr::quat_conjugate(vr::g_seatRotation);      /* the runtime's frame -> the seat's */
    const vr::Quat eyeRotation = vr::quat_mul(toSeat, vr::to_quat(view.pose.orientation));
    const vr::Vec3 eyePosition = vr::to_vec3(view.pose.position);
    vr::Vec3 eyeOffset = vr::quat_rotate(toSeat, {eyePosition.x - vr::g_seatPosition.x,
                                                  eyePosition.y - vr::g_seatPosition.y,
                                                  eyePosition.z - vr::g_seatPosition.z});
    /* The world's scale: the eyes' distance apart and the head's movements shrink as the world grows
     * (WorldScale 200%: the world is twice as big to the eyes, so the head moves half as far in it). */
    const float worldScaleFactor = 100.0f / (float)vr::setting(vr::kWorldScale);
    eyeOffset = {eyeOffset.x * worldScaleFactor, eyeOffset.y * worldScaleFactor, eyeOffset.z * worldScaleFactor};
    /* What the game needs is the inverse of that pose, as a column-major 4x4 (OpenGL's layout, as all the
     * game's matrices are): its rotation transposed, and the offset turned by that and negated.
     * rotation[row][column] is the eye's rotation matrix, from the quaternion. */
    const vr::Quat &q = eyeRotation;
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    const float rotation[3][3] = {{1 - 2 * (yy + zz), 2 * (xy - wz), 2 * (xz + wy)},
                                  {2 * (xy + wz), 1 - 2 * (xx + zz), 2 * (yz - wx)},
                                  {2 * (xz - wy), 2 * (yz + wx), 1 - 2 * (xx + yy)}};
    const float offset[3] = {eyeOffset.x, eyeOffset.y, eyeOffset.z};
    for (int column = 0; column < 3; column++) {
        /* the transpose: column c of the result is row c of `rotation' */
        for (int row = 0; row < 3; row++)
            viewInv[column * 4 + row] = rotation[column][row];
        viewInv[column * 4 + 3] = 0.0f;
    }
    for (int row = 0; row < 3; row++)       /* the translation (the fourth column): -(rotation^T x offset) */
        viewInv[12 + row] = -(rotation[0][row] * offset[0] + rotation[1][row] * offset[1] + rotation[2][row] * offset[2]);
    viewInv[15] = 1.0f;
    return true;
}

} // namespace vrview
