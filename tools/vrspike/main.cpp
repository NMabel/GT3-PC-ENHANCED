/* vrspike - Phase 0 of the VR support: does a 32-bit program get an OpenXR
 * session with Direct3D 11 on this machine's runtime (SteamVR's 32-bit one)?
 *
 * It loads the loader, creates the instance, finds the headset, makes the
 * D3D11 device on the adapter the runtime asks for, begins a session and
 * submits stereo frames (left eye red, right eye blue, so a head-mounted
 * display shows the two eyes apart) for a few seconds.  Every step prints
 * its result; the exit code is the last step that failed (0: all passed).
 *
 *   vrspike [--frames N]       default 600 (about 7 s at 90 Hz) */
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int g_step = 0;

#define CHECK(call)                                                                              \
    do {                                                                                         \
        XrResult r_ = (call);                                                                    \
        if (XR_FAILED(r_)) {                                                                     \
            char s_[XR_MAX_RESULT_STRING_SIZE] = "?";                                            \
            xrResultToString(g_instance, r_, s_);                                                \
            printf("FAIL  %s -> %s (%d)\n", #call, s_, (int)r_);                                 \
            return g_step ? g_step : 1;                                                          \
        }                                                                                        \
    } while (0)

static XrInstance g_instance = XR_NULL_HANDLE;

static void step(const char *what)
{
    g_step++;
    printf("[%d] %s\n", g_step, what);
}

struct Eye {
    XrSwapchain chain = XR_NULL_HANDLE;
    uint32_t w = 0, h = 0;
    std::vector<XrSwapchainImageD3D11KHR> images;
    std::vector<ID3D11RenderTargetView *> rtvs;
};

int main(int argc, char **argv)
{
    int frames = 600;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            frames = atoi(argv[++i]);
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("vrspike: %s, pointer size %d\n", sizeof(void *) == 4 ? "32-bit" : "64-bit", (int)sizeof(void *));

    step("instance");
    const char *exts[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(ici.applicationInfo.applicationName, "vrspike");
    ici.applicationInfo.applicationVersion = 1;
    strcpy(ici.applicationInfo.engineName, "GT3PC");
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = exts;
    CHECK(xrCreateInstance(&ici, &g_instance));
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    CHECK(xrGetInstanceProperties(g_instance, &ip));
    printf("      runtime: %s %u.%u.%u\n", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
           XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));

    step("system (head-mounted display)");
    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId sys = XR_NULL_SYSTEM_ID;
    CHECK(xrGetSystem(g_instance, &sgi, &sys));
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    CHECK(xrGetSystemProperties(g_instance, sys, &sp));
    printf("      system: %s, max layers %u, max swapchain %ux%u\n", sp.systemName, sp.graphicsProperties.maxLayerCount,
           sp.graphicsProperties.maxSwapchainImageWidth, sp.graphicsProperties.maxSwapchainImageHeight);

    step("view configuration (stereo)");
    uint32_t nv = 0;
    CHECK(xrEnumerateViewConfigurationViews(g_instance, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &nv, nullptr));
    std::vector<XrViewConfigurationView> vcv(nv, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    CHECK(xrEnumerateViewConfigurationViews(g_instance, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, nv, &nv, vcv.data()));
    for (uint32_t i = 0; i < nv; i++)
        printf("      view %u: recommended %ux%u (max %ux%u), samples %u\n", i, vcv[i].recommendedImageRectWidth,
               vcv[i].recommendedImageRectHeight, vcv[i].maxImageRectWidth, vcv[i].maxImageRectHeight,
               vcv[i].recommendedSwapchainSampleCount);

    step("D3D11 device on the runtime's adapter");
    PFN_xrGetD3D11GraphicsRequirementsKHR getReq = nullptr;
    CHECK(xrGetInstanceProcAddr(g_instance, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction *)&getReq));
    XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    CHECK(getReq(g_instance, sys, &req));
    IDXGIFactory1 *fac = nullptr;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&fac);
    IDXGIAdapter1 *ad = nullptr;
    for (UINT i = 0; fac && fac->EnumAdapters1(i, &ad) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d;
        ad->GetDesc1(&d);
        if (d.AdapterLuid.LowPart == req.adapterLuid.LowPart && d.AdapterLuid.HighPart == req.adapterLuid.HighPart) {
            printf("      adapter: %ls\n", d.Description);
            break;
        }
        ad->Release();
        ad = nullptr;
    }
    if (!ad) {
        printf("FAIL  the runtime's adapter was not found\n");
        return g_step;
    }
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(ad, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, fl, 2, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr)) {
        printf("FAIL  D3D11CreateDevice 0x%08X\n", (unsigned)hr);
        return g_step;
    }

    step("session");
    XrGraphicsBindingD3D11KHR gb{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    gb.device = dev;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb;
    sci.systemId = sys;
    XrSession session = XR_NULL_HANDLE;
    CHECK(xrCreateSession(g_instance, &sci, &session));

    step("reference space (local) and swapchains");
    XrReferenceSpaceCreateInfo rsi{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsi.poseInReferenceSpace.orientation.w = 1.0f;
    XrSpace space = XR_NULL_HANDLE;
    CHECK(xrCreateReferenceSpace(session, &rsi, &space));
    uint32_t nf = 0;
    CHECK(xrEnumerateSwapchainFormats(session, 0, &nf, nullptr));
    std::vector<int64_t> formats(nf);
    CHECK(xrEnumerateSwapchainFormats(session, nf, &nf, formats.data()));
    int64_t fmt = 0;
    printf("      formats:");
    for (int64_t f : formats)
        printf(" %d", (int)f);
    printf("\n");
    for (int64_t f : formats)
        if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) fmt = f;
    if (!fmt) fmt = formats[0];
    printf("      using format %d\n", (int)fmt);
    Eye eyes[2];
    for (int e = 0; e < 2; e++) {
        XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        sc.format = fmt;
        sc.sampleCount = 1;
        sc.width = eyes[e].w = vcv[e].recommendedImageRectWidth;
        sc.height = eyes[e].h = vcv[e].recommendedImageRectHeight;
        sc.faceCount = 1;
        sc.arraySize = 1;
        sc.mipCount = 1;
        CHECK(xrCreateSwapchain(session, &sc, &eyes[e].chain));
        uint32_t n = 0;
        CHECK(xrEnumerateSwapchainImages(eyes[e].chain, 0, &n, nullptr));
        eyes[e].images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        CHECK(xrEnumerateSwapchainImages(eyes[e].chain, n, &n, (XrSwapchainImageBaseHeader *)eyes[e].images.data()));
        for (auto &im : eyes[e].images) {
            D3D11_RENDER_TARGET_VIEW_DESC rd{};
            rd.Format = (DXGI_FORMAT)fmt;
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            ID3D11RenderTargetView *rtv = nullptr;
            dev->CreateRenderTargetView(im.texture, &rd, &rtv);
            eyes[e].rtvs.push_back(rtv);
        }
        printf("      eye %d: %ux%u, %u images\n", e, eyes[e].w, eyes[e].h, n);
    }

    step("frame loop (wait for the session to be ready)");
    bool running = false, quit = false;
    int shown = 0;
    DWORD t0 = GetTickCount();
    while (!quit && shown < frames) {
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(g_instance, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                auto *s = (XrEventDataSessionStateChanged *)&ev;
                printf("      session state -> %d\n", (int)s->state);
                if (s->state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    CHECK(xrBeginSession(session, &bi));
                    running = true;
                } else if (s->state == XR_SESSION_STATE_STOPPING) {
                    xrEndSession(session);
                    running = false;
                    quit = true;
                } else if (s->state == XR_SESSION_STATE_EXITING || s->state == XR_SESSION_STATE_LOSS_PENDING) {
                    quit = true;
                }
            }
            ev = {XR_TYPE_EVENT_DATA_BUFFER};
        }
        if (!running) {
            if (GetTickCount() - t0 > 30000) {
                printf("FAIL  the session did not become ready within 30 s\n");
                return g_step;
            }
            Sleep(20);
            continue;
        }
        XrFrameState fs{XR_TYPE_FRAME_STATE};
        CHECK(xrWaitFrame(session, nullptr, &fs));
        CHECK(xrBeginFrame(session, nullptr));
        XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                  {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        const XrCompositionLayerBaseHeader *layers[1] = {nullptr};
        uint32_t nlayers = 0;
        if (fs.shouldRender) {
            XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
            li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            li.displayTime = fs.predictedDisplayTime;
            li.space = space;
            XrViewState vs{XR_TYPE_VIEW_STATE};
            XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
            uint32_t got = 0;
            CHECK(xrLocateViews(session, &li, &vs, 2, &got, views));
            for (int e = 0; e < 2; e++) {
                uint32_t idx = 0;
                XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                CHECK(xrAcquireSwapchainImage(eyes[e].chain, &ai, &idx));
                XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wi.timeout = XR_INFINITE_DURATION;
                CHECK(xrWaitSwapchainImage(eyes[e].chain, &wi));
                const float red[4] = {0.6f, 0.05f, 0.05f, 1}, blue[4] = {0.05f, 0.05f, 0.6f, 1};
                ctx->ClearRenderTargetView(eyes[e].rtvs[idx], e == 0 ? red : blue);
                ctx->Flush();
                XrSwapchainImageReleaseInfo rl{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                CHECK(xrReleaseSwapchainImage(eyes[e].chain, &rl));
                pv[e].pose = views[e].pose;
                pv[e].fov = views[e].fov;
                pv[e].subImage.swapchain = eyes[e].chain;
                pv[e].subImage.imageRect = {{0, 0}, {(int32_t)eyes[e].w, (int32_t)eyes[e].h}};
            }
            if (shown % 90 == 0)
                printf("      frame %d: L pos (%.2f %.2f %.2f) fov L/R/U/D %.0f %.0f %.0f %.0f deg\n", shown,
                       views[0].pose.position.x, views[0].pose.position.y, views[0].pose.position.z,
                       views[0].fov.angleLeft * 57.2958f, views[0].fov.angleRight * 57.2958f,
                       views[0].fov.angleUp * 57.2958f, views[0].fov.angleDown * 57.2958f);
            layer.space = space;
            layer.viewCount = 2;
            layer.views = pv;
            layers[0] = (const XrCompositionLayerBaseHeader *)&layer;
            nlayers = 1;
        }
        XrFrameEndInfo fe{XR_TYPE_FRAME_END_INFO};
        fe.displayTime = fs.predictedDisplayTime;
        fe.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fe.layerCount = nlayers;
        fe.layers = layers;
        CHECK(xrEndFrame(session, &fe));
        shown++;
    }
    printf("PASS  %d frames submitted\n", shown);
    if (running) {
        xrRequestExitSession(session);
        Sleep(200);
    }
    xrDestroySession(session);
    xrDestroyInstance(g_instance);
    return 0;
}
