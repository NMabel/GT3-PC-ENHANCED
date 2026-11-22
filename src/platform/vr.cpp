/* vr.cpp - PC VR (OpenXR): the headset session (vr.h, vr_view.h). */
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

XrInstance g_instance = XR_NULL_HANDLE;
XrSystemId g_system = XR_NULL_SYSTEM_ID;
XrSession g_session = XR_NULL_HANDLE;
XrSpace g_local = XR_NULL_HANDLE;       /* where the player sat down (recentred by the runtime) */
XrSpace g_view = XR_NULL_HANDLE;        /* the head */
ID3D11Device *g_device;
ID3D11DeviceContext *g_context;

bool g_dead;                            /* failed or ended: no VR for the rest of the run */
bool g_running;                         /* xrBeginSession done, not ended */
bool g_frameOpen;                       /* xrBeginFrame done, xrEndFrame not yet */
bool g_shouldRender;
XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
XrTime g_displayTime;
double g_hz;
bool g_recenter = true;                 /* place the flat screen and the seat at the next rendered frame */

float g_menuDistance = 2.5f;
float g_menuWidth = 4.0f;
float g_eyeScale = 1.0f;

/* ---- the settings ([VR]) ---- */
const SettingInfo kInfo[kSettingCount] = {
    {"WorldScale", 100, 50, 200, 5},    {"HudDistance", 100, 40, 300, 5}, {"HudWidth", 70, 20, 200, 5},
    {"HudHeight", -30, -100, 100, 5},   {"MirrorDistance", 90, 40, 300, 5}, {"MirrorWidth", 30, 10, 100, 5},
    {"MirrorHeight", 20, -50, 80, 5},
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
 * The runtime offers only sRGB 8-bit swapchain formats; the game's pictures
 * are already gamma-encoded, so they are drawn through a UNORM view of the
 * same bytes (or, when the runtime's texture will not give one, through a
 * texture of ours copied over at the end of the frame). */
int64_t g_format;
DXGI_FORMAT g_unorm;

struct Image {
    ID3D11Texture2D *tex = nullptr;
    ID3D11RenderTargetView *rtv = nullptr;
};
struct Chain {
    XrSwapchain sc = XR_NULL_HANDLE;
    int w = 0, h = 0;
    std::vector<Image> images;
    ID3D11Texture2D *shadow = nullptr;  /* ours, when a view of the images' can not be had */
    ID3D11RenderTargetView *shadowRtv = nullptr;
    bool acquired = false;
    uint32_t index = 0;
};
Chain g_flat;                           /* the flat screen's quad layer */
Chain g_eye[2];                         /* the eyes of the stereo projection layer */
constexpr int kFlatW = 2560, kFlatH = 1440;
Chain g_ov[2];                          /* the dashboard's and the mirror's quad layers (vr.h overlay) */
constexpr int kOvW[2] = {2048, 1280}, kOvH[2] = {1536, 384};
float g_ovAspect[2] = {4.0f / 3.0f, 3.75f};

/* ---- the head ------------------------------------------------------------ */
struct Q {
    float x, y, z, w;
};
struct V3 {
    float x, y, z;
};
Q qmul(Q a, Q b)
{
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Q qconj(Q a) { return {-a.x, -a.y, -a.z, a.w}; }
V3 qrot(Q q, V3 v)
{
    /* v' = q v q* */
    const Q p = qmul(qmul(q, {v.x, v.y, v.z, 0.0f}), qconj(q));
    return {p.x, p.y, p.z};
}
Q to_q(const XrQuaternionf &q) { return {q.x, q.y, q.z, q.w}; }
V3 to_v(const XrVector3f &v) { return {v.x, v.y, v.z}; }

XrPosef g_quadPose = {{0, 0, 0, 1}, {0, 0, -2.5f}};
Q g_refQ = {0, 0, 0, 1};                /* the seat: where the head was at the last recentre (yaw only) */
V3 g_refP = {0, 0, 0};

XrView g_views[2];                      /* this frame's eyes, in the local space */
bool g_viewsValid;
bool g_stereoFrame;                     /* vrview::frame */

float ini_float(const char *key, float def, float lo, float hi)
{
    char buf[32] = "";
    GetPrivateProfileStringA("VR", key, "", buf, sizeof(buf), paths::ini_path().c_str());
    if (!buf[0])
        return def;
    const float v = (float)atof(buf);
    return v < lo ? lo : v > hi ? hi : v;
}

/* A failed call: logged (the result's name), and the session ended for
 * good when it is the kind that does not come back. */
bool ok(XrResult r, const char *what)
{
    if (XR_SUCCEEDED(r))
        return true;
    char s[XR_MAX_RESULT_STRING_SIZE] = "?";
    if (g_instance)
        xrResultToString(g_instance, r, s);
    pclog::write("vr: %s failed: %s (%d)", what, s, (int)r);
    if (r == XR_ERROR_SESSION_LOST || r == XR_ERROR_INSTANCE_LOST || r == XR_ERROR_RUNTIME_FAILURE)
        g_dead = true;
    return false;
}

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

/* The flat screen's place and the seat: the head's place, level, facing it. */
void recenter(XrTime time)
{
    XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(g_view, g_local, time, &loc)))
        return;
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if ((loc.locationFlags & need) != need)
        return;
    const Q q = to_q(loc.pose.orientation);
    /* the head's forward (-z) turned by q, then flattened */
    const float fx = -2.0f * (q.x * q.z + q.w * q.y);
    const float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    const float len = sqrtf(fx * fx + fz * fz);
    if (len < 0.01f)
        return;                         /* looking straight up or down: try again */
    const float dx = fx / len, dz = fz / len;
    const float theta = atan2f(-dx, -dz);
    g_quadPose.orientation = {0.0f, sinf(theta * 0.5f), 0.0f, cosf(theta * 0.5f)};
    g_quadPose.position = {loc.pose.position.x + dx * g_menuDistance, loc.pose.position.y,
                           loc.pose.position.z + dz * g_menuDistance};
    g_refQ = {0.0f, sinf(theta * 0.5f), 0.0f, cosf(theta * 0.5f)};
    g_refP = to_v(loc.pose.position);
    g_recenter = false;
    pclog::write("vr: centred: the head at (%.2f %.2f %.2f), facing %.0f degrees", g_refP.x, g_refP.y, g_refP.z,
                 theta * 57.29578f);
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
    if (c.sc)
        xrDestroySwapchain(c.sc);
    c.sc = XR_NULL_HANDLE;
    c.acquired = false;
}

bool pick_format()
{
    uint32_t n = 0;
    if (!ok(xrEnumerateSwapchainFormats(g_session, 0, &n, nullptr), "xrEnumerateSwapchainFormats"))
        return false;
    std::vector<int64_t> formats(n);
    if (!ok(xrEnumerateSwapchainFormats(g_session, n, &n, formats.data()), "xrEnumerateSwapchainFormats"))
        return false;
    g_format = 0;
    for (int64_t f : formats)
        if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
            g_format = f;
    if (!g_format)
        for (int64_t f : formats)
            if (f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
                g_format = f;
    if (!g_format) {
        pclog::write("vr: no 8-bit sRGB swapchain format offered");
        return false;
    }
    g_unorm = g_format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
    return true;
}

bool make_chain(Chain &c, int w, int h, const char *what)
{
    XrSwapchainCreateInfo sc = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                    XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sc.format = g_format;
    sc.sampleCount = 1;
    sc.width = (uint32_t)w;
    sc.height = (uint32_t)h;
    sc.faceCount = 1;
    sc.arraySize = 1;
    sc.mipCount = 1;
    char label[64];
    snprintf(label, sizeof(label), "xrCreateSwapchain (%s)", what);
    if (!ok(xrCreateSwapchain(g_session, &sc, &c.sc), label)) {
        c.sc = XR_NULL_HANDLE;
        return false;
    }
    c.w = w;
    c.h = h;
    uint32_t count = 0;
    if (!ok(xrEnumerateSwapchainImages(c.sc, 0, &count, nullptr), "xrEnumerateSwapchainImages"))
        return false;
    std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (!ok(xrEnumerateSwapchainImages(c.sc, count, &count, (XrSwapchainImageBaseHeader *)images.data()),
            "xrEnumerateSwapchainImages"))
        return false;
    bool viewed = true;
    for (auto &im : images) {
        Image i;
        i.tex = im.texture;
        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format = g_unorm;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(g_device->CreateRenderTargetView(i.tex, &rd, &i.rtv))) {
            viewed = false;
            break;
        }
        c.images.push_back(i);
    }
    if (!viewed) {
        /* a texture of ours (UNORM), copied into the swapchain's at the end of the frame */
        for (auto &i : c.images)
            if (i.rtv)
                i.rtv->Release();
        c.images.clear();
        for (auto &im : images) {
            Image i;
            i.tex = im.texture;
            c.images.push_back(i);
        }
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = (UINT)w;
        td.Height = (UINT)h;
        td.MipLevels = td.ArraySize = 1;
        td.Format = g_unorm;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(g_device->CreateTexture2D(&td, nullptr, &c.shadow)) ||
            FAILED(g_device->CreateRenderTargetView(c.shadow, nullptr, &c.shadowRtv))) {
            pclog::write("vr: the %s's render target could not be made", what);
            return false;
        }
        pclog::write("vr: the %s is drawn through a copy (the runtime's texture has no UNORM view)", what);
    }
    pclog::write("vr: %s %dx%d, %u images, format %d", what, w, h, count, (int)g_format);
    return true;
}

bool acquire(Chain &c, ID3D11RenderTargetView **rtv)
{
    if (c.sc == XR_NULL_HANDLE || c.acquired)
        return false;
    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!ok(xrAcquireSwapchainImage(c.sc, &ai, &c.index), "xrAcquireSwapchainImage"))
        return false;
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (!ok(xrWaitSwapchainImage(c.sc, &wi), "xrWaitSwapchainImage")) {
        XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(c.sc, &ri);
        return false;
    }
    c.acquired = true;
    *rtv = c.shadowRtv ? c.shadowRtv : c.images[c.index].rtv;
    return true;
}

/* What was drawn onto the image is handed back to the runtime (its commands
 * have been given to the device: the D3D thread was idle). */
void release(Chain &c)
{
    if (!c.acquired)
        return;
    if (c.shadow)
        g_context->CopyResource(c.images[c.index].tex, c.shadow);
    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    ok(xrReleaseSwapchainImage(c.sc, &ri), "xrReleaseSwapchainImage");
    c.acquired = false;
}

/* xrWaitFrame + xrBeginFrame: the next frame's time and whether it is drawn,
 * and the head where it will be then. */
void next_frame()
{
    g_viewsValid = false;
    XrFrameState fs = {XR_TYPE_FRAME_STATE};
    if (!ok(xrWaitFrame(g_session, nullptr, &fs), "xrWaitFrame"))
        return;
    if (!ok(xrBeginFrame(g_session, nullptr), "xrBeginFrame"))
        return;
    g_frameOpen = true;
    g_shouldRender = fs.shouldRender != 0;
    g_displayTime = fs.predictedDisplayTime;
    if (fs.predictedDisplayPeriod > 0)
        g_hz = 1.0e9 / (double)fs.predictedDisplayPeriod;
    if (!g_shouldRender)
        return;
    if (g_recenter)
        recenter(g_displayTime);
    XrViewLocateInfo li = {XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = g_displayTime;
    li.space = g_local;
    XrViewState vs = {XR_TYPE_VIEW_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t got = 0;
    if (XR_FAILED(xrLocateViews(g_session, &li, &vs, 2, &got, views)) || got != 2)
        return;
    const XrViewStateFlags need = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    if ((vs.viewStateFlags & need) != need)
        return;
    g_views[0] = views[0];
    g_views[1] = views[1];
    g_viewsValid = true;
}

void poll_events()
{
    for (;;) {
        XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
        if (xrPollEvent(g_instance, &ev) != XR_SUCCESS)
            return;
        switch (ev.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            const auto *s = (const XrEventDataSessionStateChanged *)&ev;
            const XrSessionState before = g_state;
            g_state = s->state;
            pclog::write("vr: session state %d", (int)s->state);
            /* the headset put on again (SteamVR sleeps the screens off the head: the
             * session falls back to synchronized): the flat screen and the seat are placed anew */
            if (before == XR_SESSION_STATE_SYNCHRONIZED && s->state == XR_SESSION_STATE_VISIBLE)
                g_recenter = true;
            if (s->state == XR_SESSION_STATE_READY && !g_running) {
                XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (ok(xrBeginSession(g_session, &bi), "xrBeginSession")) {
                    g_running = true;
                    next_frame();
                }
            } else if (s->state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(g_session);
                quit_session("the runtime stopped it");
            } else if (s->state == XR_SESSION_STATE_EXITING || s->state == XR_SESSION_STATE_LOSS_PENDING) {
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

/* The VR hotkeys while the game's window has not the keyboard: the player is in the headset, the keys
 * are read from the system (the window's own hotkeys act while it has it: window.cpp).  The world's
 * scale keys repeat while held. */
void poll_hotkeys()
{
    static bool down[5];
    static ULONGLONG next[5];
    const ULONGLONG now = GetTickCount64();
    const bool ctrl = GetAsyncKeyState(VK_CONTROL) < 0, alt = GetAsyncKeyState(VK_MENU) < 0,
               shift = GetAsyncKeyState(VK_SHIFT) < 0;
    for (int i = 0; i < 5; i++) {
        const input_map::KeyCombo &k = input_map::hotkey(input_map::kHotVrRecenter + i);
        const bool pressed = k.vk != 0 && (GetAsyncKeyState(k.vk) & 0x8000) != 0 && k.ctrl == ctrl && k.alt == alt &&
                             k.shift == shift;
        const bool scale = i >= 3;
        if (pressed && (!down[i] || (scale && now >= next[i]))) {
            next[i] = now + (down[i] ? 90 : 400);
            hotkey((Action)i);
        }
        down[i] = pressed;
    }
}

/* ---- the overlays' own drawing: a part of the game's overlay picture onto a panel's swapchain ---- */
const char kOverlayHlsl[] = R"HLSL(
cbuffer C : register(b0) { float4 uvRect; float4 params; };
struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VO vs_ov(uint id : SV_VertexID)
{
    float2 p = float2((id << 1) & 2, id & 2);
    VO o;
    o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
    o.uv = lerp(uvRect.xy, uvRect.zw, p);
    return o;
}
Texture2D tex : register(t0);
SamplerState smp : register(s0);
float4 ps_ov(VO i) : SV_Target
{
    float4 c = tex.Sample(smp, i.uv);
    /* the dashboard: the black of the picture is clear (params.x 1), brighter and brighter is more there */
    float a = params.x > 0.5 ? saturate(max(c.r, max(c.g, c.b)) * params.y) : 1.0;
    return float4(c.rgb, a);
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
    bd.ByteWidth = 32;
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

/* A panel of the cockpit: `from' the seat (the head's place when the view was centred, facing forward): its
 * distance in front, its height from the eyes (metres), its quad turned to the seat. */
XrPosef seat_panel(float dist, float height)
{
    const Q yawq = g_refQ;
    const V3 off = qrot(yawq, {0.0f, height, -dist});
    const float pitch = atan2f(height, dist);
    const Q qx = {sinf(pitch * 0.5f), 0.0f, 0.0f, cosf(pitch * 0.5f)};
    const Q o = qmul(yawq, qx);
    XrPosef p;
    p.orientation = {o.x, o.y, o.z, o.w};
    p.position = {g_refP.x + off.x, g_refP.y + off.y, g_refP.z + off.z};
    return p;
}

} // namespace

bool wanted()
{
    char on[16] = "";
    GetPrivateProfileStringA("VR", "Enabled", "off", on, sizeof(on), paths::ini_path().c_str());
    return _stricmp(on, "on") == 0 || strcmp(on, "1") == 0;
}

bool prepare(LUID *adapter)
{
    if (!wanted())
        return false;
    if (LoadLibraryA("openxr_loader.dll") == nullptr) {
        pclog::write("vr: openxr_loader.dll was not found next to the game: no VR");
        return false;
    }
    uint32_t n = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
    std::vector<XrExtensionProperties> exts(n, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, n, &n, exts.data());
    bool d3d11 = false;
    for (auto &e : exts)
        if (strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0)
            d3d11 = true;
    if (!d3d11) {
        pclog::write("vr: the OpenXR runtime has no Direct3D 11 support: no VR");
        return false;
    }
    const char *want[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(ici.applicationInfo.applicationName, "Gran Turismo 3 PC");
    ici.applicationInfo.applicationVersion = 1;
    strcpy(ici.applicationInfo.engineName, "GT3PC");
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = want;
    XrResult r = xrCreateInstance(&ici, &g_instance);
    if (XR_FAILED(r)) {
        g_instance = XR_NULL_HANDLE;
        pclog::write("vr: no OpenXR runtime could be started (%d): no VR", (int)r);
        return false;
    }
    XrInstanceProperties ip = {XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(g_instance, &ip);
    pclog::write("vr: runtime %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
                 XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));
    XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = xrGetSystem(g_instance, &sgi, &g_system);
    if (XR_FAILED(r)) {
        pclog::write("vr: no headset found (%d; is it on, with SteamVR running?): no VR", (int)r);
        xrDestroyInstance(g_instance);
        g_instance = XR_NULL_HANDLE;
        return false;
    }
    XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(g_instance, g_system, &sp);
    pclog::write("vr: headset system \"%s\"", sp.systemName);
    PFN_xrGetD3D11GraphicsRequirementsKHR getReq = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(g_instance, "xrGetD3D11GraphicsRequirementsKHR",
                                        (PFN_xrVoidFunction *)&getReq)) ||
        getReq == nullptr) {
        pclog::write("vr: xrGetD3D11GraphicsRequirementsKHR is missing: no VR");
        return false;
    }
    XrGraphicsRequirementsD3D11KHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (!ok(getReq(g_instance, g_system, &req), "xrGetD3D11GraphicsRequirementsKHR"))
        return false;
    *adapter = req.adapterLuid;
    ensure_loaded();
    g_menuDistance = ini_float("MenuDistance", 2.5f, 0.8f, 10.0f);
    g_menuWidth = ini_float("MenuWidth", 4.0f, 0.5f, 20.0f);
    g_eyeScale = ini_float("RenderScale", 1.0f, 0.5f, 2.0f);
    /* MenuFov: the screen's width as an angle (THX recommends about 40 degrees, SMPTE 30):
     * the width follows from the distance */
    const float fov = ini_float("MenuFov", 0.0f, 0.0f, 120.0f);
    if (fov >= 5.0f)
        g_menuWidth = 2.0f * g_menuDistance * tanf(fov * 0.5f * 3.14159265f / 180.0f);
    return true;
}

bool start(ID3D11Device *device)
{
    if (g_instance == XR_NULL_HANDLE)
        return false;
    g_device = device;
    g_device->GetImmediateContext(&g_context);
    XrGraphicsBindingD3D11KHR gb = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    gb.device = device;
    XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb;
    sci.systemId = g_system;
    if (!ok(xrCreateSession(g_instance, &sci, &g_session), "xrCreateSession")) {
        g_session = XR_NULL_HANDLE;
        g_dead = true;
        return false;
    }
    XrReferenceSpaceCreateInfo rsi = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsi.poseInReferenceSpace = pose_identity();
    rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    bool good = ok(xrCreateReferenceSpace(g_session, &rsi, &g_local), "xrCreateReferenceSpace (local)");
    rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    good = good && ok(xrCreateReferenceSpace(g_session, &rsi, &g_view), "xrCreateReferenceSpace (view)");
    if (!good || !pick_format() || !make_chain(g_flat, kFlatW, kFlatH, "flat screen")) {
        shutdown();
        g_dead = true;
        return false;
    }
    /* the eyes: the runtime's recommended size, scaled ([VR] RenderScale) */
    uint32_t nv = 0;
    std::vector<XrViewConfigurationView> vcv;
    if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                       0, &nv, nullptr)) &&
        nv == 2) {
        vcv.assign(nv, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
        xrEnumerateViewConfigurationViews(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, nv, &nv,
                                          vcv.data());
        const int w = std::min((int)vcv[0].maxImageRectWidth,
                               std::max(256, (int)((float)vcv[0].recommendedImageRectWidth * g_eyeScale + 0.5f)));
        const int h = std::min((int)vcv[0].maxImageRectHeight,
                               std::max(256, (int)((float)vcv[0].recommendedImageRectHeight * g_eyeScale + 0.5f)));
        if (!make_chain(g_eye[0], w, h, "left eye") || !make_chain(g_eye[1], w, h, "right eye")) {
            free_chain(g_eye[0]);
            free_chain(g_eye[1]);
            pclog::write("vr: no eye swapchains: the game stays on the flat screen");
        }
    }
    /* the dashboard's and the mirror's panels */
    for (int k = 0; k < 2; k++)
        if (!make_chain(g_ov[k], kOvW[k], kOvH[k], k == 0 ? "dashboard panel" : "mirror panel")) {
            free_chain(g_ov[k]);
            pclog::write("vr: no %s swapchain: it is not shown", k == 0 ? "dashboard" : "mirror");
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
        free_chain(g_ov[0]);
        free_chain(g_ov[1]);
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
    return active() && g_frameOpen && g_shouldRender && g_viewsValid && g_eye[0].sc != XR_NULL_HANDLE &&
           g_eye[1].sc != XR_NULL_HANDLE;
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
        g_context->Flush();
        XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                  {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        XrCompositionLayerProjection proj = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        XrCompositionLayerQuad quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        XrCompositionLayerQuad ovq[2] = {{XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD}};
        const XrCompositionLayerBaseHeader *layers[4] = {nullptr, nullptr, nullptr, nullptr};
        uint32_t count = 0;
        const bool eyes = g_eye[0].acquired && g_eye[1].acquired && g_viewsValid;
        for (int e = 0; e < 2; e++)
            release(g_eye[e]);
        if (eyes) {
            for (int e = 0; e < 2; e++) {
                pv[e].pose = g_views[e].pose;
                pv[e].fov = g_views[e].fov;
                pv[e].subImage.swapchain = g_eye[e].sc;
                pv[e].subImage.imageRect = {{0, 0}, {g_eye[e].w, g_eye[e].h}};
            }
            proj.layerFlags = 0;
            proj.space = g_local;
            proj.viewCount = 2;
            proj.views = pv;
            layers[count++] = (const XrCompositionLayerBaseHeader *)&proj;
        }
        /* the mirror, then the dashboard over it: panels of the cockpit, where the seat is */
        static const int kOrder[2] = {kOverlayMirror, kOverlayHud};
        for (int oi = 0; oi < 2; oi++) {
            const int k = kOrder[oi];
            if (!g_ov[k].acquired)
                continue;
            release(g_ov[k]);
            const float dist = (float)setting(k == kOverlayHud ? kHudDistance : kMirrorDistance) / 100.0f;
            const float width = (float)setting(k == kOverlayHud ? kHudWidth : kMirrorWidth) / 100.0f;
            const float height = (float)setting(k == kOverlayHud ? kHudHeight : kMirrorHeight) / 100.0f;
            XrCompositionLayerQuad &q = ovq[oi];
            q.layerFlags = k == kOverlayHud ? (XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                               XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT)
                                            : 0;
            q.space = g_local;
            q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            q.subImage.swapchain = g_ov[k].sc;
            q.subImage.imageRect = {{0, 0}, {g_ov[k].w, g_ov[k].h}};
            q.pose = seat_panel(dist, height);
            q.size = {width, width / g_ovAspect[k]};
            if (eyes)
                layers[count++] = (const XrCompositionLayerBaseHeader *)&q;
        }
        if (g_flat.acquired) {
            release(g_flat);
            quad.layerFlags = 0;
            quad.space = g_local;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = g_flat.sc;
            quad.subImage.imageRect = {{0, 0}, {g_flat.w, g_flat.h}};
            quad.pose = g_quadPose;
            quad.size = {g_menuWidth, g_menuWidth * (float)g_flat.h / (float)g_flat.w};
            layers[count++] = (const XrCompositionLayerBaseHeader *)&quad;
        }
        XrFrameEndInfo fe = {XR_TYPE_FRAME_END_INFO};
        fe.displayTime = g_displayTime;
        fe.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fe.layerCount = count;
        fe.layers = layers;
        ok(xrEndFrame(g_session, &fe), "xrEndFrame");
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
    return (kind == kOverlayHud ? hud_on() : mirror_on()) && g_ov[kind].sc != XR_NULL_HANDLE;
}

bool overlay_acquire(int kind, float aspect, ID3D11RenderTargetView **rtv, int *width, int *height)
{
    if ((kind != 0 && kind != 1) || !overlay_wanted(kind) || !active() || !g_frameOpen || !g_shouldRender)
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
    float *c = (float *)m.pData;
    c[0] = uv[0];
    c[1] = uv[1];
    c[2] = uv[2];
    c[3] = uv[3];
    c[4] = kind == kOverlayHud ? 1.0f : 0.0f;
    c[5] = 4.0f;
    c[6] = c[7] = 0.0f;
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

bool eye(int e, float tan[4], float viewInv[16])
{
    if ((e != 0 && e != 1) || !vr::g_viewsValid)
        return false;
    const XrView &v = vr::g_views[e];
    tan[0] = tanf(v.fov.angleLeft);
    tan[1] = tanf(v.fov.angleRight);
    tan[2] = tanf(v.fov.angleDown);
    tan[3] = tanf(v.fov.angleUp);
    /* the eye's pose in the seat's frame: the head's place and turn relative to the centred ones */
    const vr::Q inv = vr::qconj(vr::g_refQ);
    const vr::Q qd = vr::qmul(inv, vr::to_q(v.pose.orientation));
    const vr::V3 p = vr::to_v(v.pose.position);
    vr::V3 t = vr::qrot(inv, {p.x - vr::g_refP.x, p.y - vr::g_refP.y, p.z - vr::g_refP.z});
    /* the world's scale: the eyes' distance and the head's movements shrink as the world grows */
    const float k = 100.0f / (float)vr::setting(vr::kWorldScale);
    t = {t.x * k, t.y * k, t.z * k};
    /* its rotation matrix R (rows), then the inverse pose: R transposed, - R^T t (column-major) */
    const float xx = qd.x * qd.x, yy = qd.y * qd.y, zz = qd.z * qd.z;
    const float xy = qd.x * qd.y, xz = qd.x * qd.z, yz = qd.y * qd.z;
    const float wx = qd.w * qd.x, wy = qd.w * qd.y, wz = qd.w * qd.z;
    const float R[3][3] = {{1 - 2 * (yy + zz), 2 * (xy - wz), 2 * (xz + wy)},
                           {2 * (xy + wz), 1 - 2 * (xx + zz), 2 * (yz - wx)},
                           {2 * (xz - wy), 2 * (yz + wx), 1 - 2 * (xx + yy)}};
    const float tv[3] = {t.x, t.y, t.z};
    for (int c = 0; c < 3; c++) {
        for (int r = 0; r < 3; r++)
            viewInv[c * 4 + r] = R[c][r];
        viewInv[c * 4 + 3] = 0.0f;
    }
    for (int r = 0; r < 3; r++)
        viewInv[12 + r] = -(R[0][r] * tv[0] + R[1][r] * tv[1] + R[2][r] * tv[2]);
    viewInv[15] = 1.0f;
    return true;
}

} // namespace vrview
