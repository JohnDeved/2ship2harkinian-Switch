#include "fast/Fast3dWindow.h"

#include "ship/Context.h"
#include "ship/config/Config.h"
#include "ship/controller/controldeck/ControlDeck.h"
#include "ship/config/ConsoleVariable.h"
#include "fast/interpreter.h"
#include "fast/backends/gfx_sdl.h"
#include "fast/backends/gfx_dxgi.h"
#include "fast/backends/gfx_opengl.h"
#include "fast/backends/gfx_metal.h"
#include "fast/backends/gfx_direct3d_common.h"
#include "fast/backends/gfx_direct3d11.h"
#include "fast/backends/gfx_window_manager_api.h"

#ifdef __SWITCH__
#include "ship/port/switch/SwitchImpl.h"
#endif

#include <fstream>
#include <imgui.h>

#ifdef ENABLE_OPENGL
#include <imgui_impl_opengl3.h>
#endif

namespace Fast {

extern void GfxSetInstance(std::shared_ptr<Interpreter> gfx);

Fast3dWindow::Fast3dWindow(std::shared_ptr<Ship::Gui> gui) : Ship::Window(gui) {
    mWindowManagerApi = nullptr;
    mRenderingApi = nullptr;
    mInterpreter = std::make_shared<Interpreter>();
    GfxSetInstance(mInterpreter);

#ifdef _WIN32
    AddAvailableWindowBackend(Ship::WindowBackend::FAST3D_DXGI_DX11);
#endif
#ifdef __APPLE__
    if (Metal_IsSupported()) {
        AddAvailableWindowBackend(Ship::WindowBackend::FAST3D_SDL_METAL);
    }
#endif
    AddAvailableWindowBackend(Ship::WindowBackend::FAST3D_SDL_OPENGL);
}

Fast3dWindow::Fast3dWindow(std::vector<std::shared_ptr<Ship::GuiWindow>> guiWindows)
    : Fast3dWindow(std::make_shared<Ship::Gui>(guiWindows)) {
}

Fast3dWindow::Fast3dWindow() : Fast3dWindow(std::vector<std::shared_ptr<Ship::GuiWindow>>()) {
}

Fast3dWindow::~Fast3dWindow() {
    SPDLOG_DEBUG("destruct fast3dwindow");
#ifdef __SWITCH__
    DestroyRenderThread();
#endif
    // Clear raw sInstance pointer before destroying the interpreter to prevent
    // use-after-free if any code tries to use sInstance after this point.
    GfxSetInstance(nullptr);
    mInterpreter->Destroy();
    delete mRenderingApi;
    delete mWindowManagerApi;
}

void Fast3dWindow::Init() {
    bool gameMode = false;

#ifdef __linux__
    std::ifstream osReleaseFile("/etc/os-release");
    if (osReleaseFile.is_open()) {
        std::string line;
        while (std::getline(osReleaseFile, line)) {
            if (line.find("VARIANT_ID") != std::string::npos) {
                if (line.find("steamdeck") != std::string::npos) {
                    gameMode = std::getenv("XDG_CURRENT_DESKTOP") != nullptr &&
                               std::string(std::getenv("XDG_CURRENT_DESKTOP")) == "gamescope";
                }
                break;
            }
        }
    }
#elif defined(__ANDROID__) || defined(__IOS__)
    gameMode = true;
#endif

    bool isFullscreen;
    uint32_t width, height;
    int32_t posX, posY;

    isFullscreen = Ship::Context::GetInstance()->GetConfig()->GetBool("Window.Fullscreen.Enabled", false) || gameMode;
    posX = Ship::Context::GetInstance()->GetConfig()->GetInt("Window.PositionX", 100);
    posY = Ship::Context::GetInstance()->GetConfig()->GetInt("Window.PositionY", 100);

    if (isFullscreen) {
        width = Ship::Context::GetInstance()->GetConfig()->GetInt("Window.Fullscreen.Width", gameMode ? 1280 : 1920);
        height = Ship::Context::GetInstance()->GetConfig()->GetInt("Window.Fullscreen.Height", gameMode ? 800 : 1080);
    } else {
        width = Ship::Context::GetInstance()->GetConfig()->GetInt("Window.Width", 640);
        height = Ship::Context::GetInstance()->GetConfig()->GetInt("Window.Height", 480);
    }
    Ship::Context::GetInstance()->GetWindow()->SetFullscreenScancode(
        Ship::Context::GetInstance()->GetConfig()->GetInt("Shortcuts.Fullscreen", Ship::KbScancode::LUS_KB_F11));
    Ship::Context::GetInstance()->GetWindow()->SetMouseCaptureScancode(
        Ship::Context::GetInstance()->GetConfig()->GetInt("Shortcuts.MouseCapture", Ship::KbScancode::LUS_KB_F2));

    InitWindowManager();
    mInterpreter->Init(mWindowManagerApi, mRenderingApi, Ship::Context::GetInstance()->GetName().c_str(), isFullscreen,
                       width, height, posX, posY);
    mWindowManagerApi->SetFullscreenChangedCallback(OnFullscreenChanged);
    mWindowManagerApi->SetKeyboardCallbacks(KeyDown, KeyUp, AllKeysUp);
    mWindowManagerApi->SetMouseCallbacks(MouseButtonDown, MouseButtonUp);

    SetTextureFilter((FilteringMode)Ship::Context::GetInstance()->GetConsoleVariables()->GetInteger(
        CVAR_TEXTURE_FILTER, FILTER_THREE_POINT));
}

int32_t Fast3dWindow::GetTargetFps() {
    return mInterpreter->GetTargetFps();
}

void Fast3dWindow::SetTargetFps(int32_t fps) {
    mInterpreter->SetTargetFps(fps);
}

void Fast3dWindow::SetMaximumFrameLatency(int32_t latency) {
    mInterpreter->SetMaxFrameLatency(latency);
}

void Fast3dWindow::GetPixelDepthPrepare(float x, float y) {
    mInterpreter->GetPixelDepthPrepare(x, y);
}

uint16_t Fast3dWindow::GetPixelDepth(float x, float y) {
    return mInterpreter->GetPixelDepth(x, y);
}

void Fast3dWindow::InitWindowManager() {
    SetWindowBackend(Ship::Context::GetInstance()->GetConfig()->GetWindowBackend());

    switch (GetWindowBackend()) {
#ifdef ENABLE_DX11
        case Ship::WindowBackend::FAST3D_DXGI_DX11:
            mWindowManagerApi = new GfxWindowBackendDXGI();
            mRenderingApi = new GfxRenderingAPIDX11(static_cast<GfxWindowBackendDXGI*>(mWindowManagerApi));
            break;
#endif
#ifdef ENABLE_OPENGL
        case Ship::WindowBackend::FAST3D_SDL_OPENGL:
            mRenderingApi = new GfxRenderingAPIOGL();
            mWindowManagerApi = new GfxWindowBackendSDL2();
            break;
#endif
#ifdef __APPLE__
        case Ship::WindowBackend::FAST3D_SDL_METAL:
            mRenderingApi = new GfxRenderingAPIMetal();
            mWindowManagerApi = new GfxWindowBackendSDL2();
            break;
#endif
        default:
            SPDLOG_ERROR("Could not load the correct rendering backend");
            break;
    }
}

void Fast3dWindow::SetTextureFilter(FilteringMode filteringMode) {
    mInterpreter->GetCurrentRenderingAPI()->SetTextureFilter(filteringMode);
}

void Fast3dWindow::EnableSRGBMode() {
    mInterpreter->mRapi->SetSrgbMode();
}

void Fast3dWindow::SetRendererUCode(UcodeHandlers ucode) {
    gfx_set_target_ucode(ucode);
}

void Fast3dWindow::Close() {
    mWindowManagerApi->Close();
}

void Fast3dWindow::RunGuiOnly() {
    mInterpreter->RunGuiOnly();
}

void Fast3dWindow::StartFrame() {
    mInterpreter->StartFrame();
}

void Fast3dWindow::EndFrame() {
    mInterpreter->EndFrame();
}

bool Fast3dWindow::IsFrameReady() {
    return mWindowManagerApi->IsFrameReady();
}

bool Fast3dWindow::DrawAndRunGraphicsCommands(Gfx* commands, const std::unordered_map<Mtx*, MtxF>& mtxReplacements) {
    std::shared_ptr<Window> wnd = Ship::Context::GetInstance()->GetWindow();

    // Skip dropped frames
    if (!wnd->IsFrameReady()) {
        return false;
    }

    auto gui = wnd->GetGui();
    // Setup of the backend frames and draw initial Window and GUI menus
    gui->StartDraw();
    // Setup game framebuffers to match available window space
    mInterpreter->StartFrame();
    // Execute the games gfx commands
    mInterpreter->Run(commands, mtxReplacements);
    // Renders the game frame buffer to the final window and finishes the GUI
    gui->EndDraw();
    // Finalize swap buffers
    mInterpreter->EndFrame();

    return true;
}

const Fast3DStats& Fast3dWindow::GetFrameStats() const {
    return mInterpreter->GetFrameStats();
}

void Fast3dWindow::ResetFrameStats() {
    mInterpreter->ResetFrameStats();
}

void Fast3dWindow::SetProfilingEnabled(bool enabled) {
    mInterpreter->SetProfilingEnabled(enabled);
}

void Fast3dWindow::HandleEvents() {
    mWindowManagerApi->HandleEvents();
}

void Fast3dWindow::SetCursorVisibility(bool visible) {
    mWindowManagerApi->SetCursorVisibility(visible);
}

uint32_t Fast3dWindow::GetWidth() {
    uint32_t width, height;
    int32_t posX, posY;
    mWindowManagerApi->GetDimensions(&width, &height, &posX, &posY);
    return width;
}

uint32_t Fast3dWindow::GetHeight() {
    uint32_t width, height;
    int32_t posX, posY;
    mWindowManagerApi->GetDimensions(&width, &height, &posX, &posY);
    return height;
}

float Fast3dWindow::GetAspectRatio() {
    return mInterpreter->mCurDimensions.aspect_ratio;
}

int32_t Fast3dWindow::GetPosX() {
    uint32_t width, height;
    int32_t posX, posY;
    mWindowManagerApi->GetDimensions(&width, &height, &posX, &posY);
    return posX;
}

int32_t Fast3dWindow::GetPosY() {
    uint32_t width, height;
    int32_t posX, posY;
    mWindowManagerApi->GetDimensions(&width, &height, &posX, &posY);
    return posY;
}

void Fast3dWindow::SetMousePos(Ship::Coords pos) {
    mWindowManagerApi->SetMousePos(pos.x, pos.y);
}

Ship::Coords Fast3dWindow::GetMousePos() {
    int32_t x, y;
    mWindowManagerApi->GetMousePos(&x, &y);
    return { x, y };
}

Ship::Coords Fast3dWindow::GetMouseDelta() {
    int32_t x, y;
    mWindowManagerApi->GetMouseDelta(&x, &y);
    return { x, y };
}

Ship::CoordsF Fast3dWindow::GetMouseWheel() {
    float x, y;
    mWindowManagerApi->GetMouseWheel(&x, &y);
    return { x, y };
}

bool Fast3dWindow::GetMouseState(Ship::MouseBtn btn) {
    return mWindowManagerApi->GetMouseState(static_cast<uint32_t>(btn));
}

void Fast3dWindow::SetMouseCapture(bool capture) {
    mWindowManagerApi->SetMouseCapture(capture);
}

bool Fast3dWindow::IsMouseCaptured() {
    return mWindowManagerApi->IsMouseCaptured();
}

uint32_t Fast3dWindow::GetCurrentRefreshRate() {
    uint32_t refreshRate;
    mWindowManagerApi->GetActiveWindowRefreshRate(&refreshRate);
    return refreshRate;
}

bool Fast3dWindow::SupportsWindowedFullscreen() {
#if defined(__APPLE__) || defined(__SWITCH__)
    return false;
#endif

    if (GetWindowBackend() == Ship::WindowBackend::FAST3D_SDL_OPENGL) {
        return true;
    }

    return false;
}

bool Fast3dWindow::CanDisableVerticalSync() {
    return mWindowManagerApi->CanDisableVsync();
}

void Fast3dWindow::SetResolutionMultiplier(float multiplier) {
    mInterpreter->SetResolutionMultiplier(multiplier);
}

void Fast3dWindow::SetMsaaLevel(uint32_t value) {
    mInterpreter->SetMsaaLevel(value);
}

void Fast3dWindow::SetFullscreen(bool isFullscreen) {
    // Save current window position before fullscreening
    SaveWindowToConfig();
    mWindowManagerApi->SetFullscreen(isFullscreen);
}

bool Fast3dWindow::IsFullscreen() {
    return mWindowManagerApi->IsFullscreen();
}

bool Fast3dWindow::IsRunning() {
    return mWindowManagerApi->IsRunning();
}

uintptr_t Fast3dWindow::GetGfxFrameBuffer() {
    return mInterpreter->mGfxFrameBuffer;
}

const char* Fast3dWindow::GetKeyName(int32_t scancode) {
    return mWindowManagerApi->GetKeyName(scancode);
}

bool Fast3dWindow::KeyUp(int32_t scancode) {
    if (scancode == Ship::Context::GetInstance()->GetWindow()->GetFullscreenScancode()) {
        Ship::Context::GetInstance()->GetWindow()->ToggleFullscreen();
    }

    if (scancode == Ship::Context::GetInstance()->GetWindow()->GetMouseCaptureScancode()) {
        bool captureState = Ship::Context::GetInstance()->GetWindow()->IsMouseCaptured();
        Ship::Context::GetInstance()->GetWindow()->SetMouseCapture(!captureState);
    }

    Ship::Context::GetInstance()->GetWindow()->SetLastScancode(-1);
    return Ship::Context::GetInstance()->GetControlDeck()->ProcessKeyboardEvent(
        Ship::KbEventType::LUS_KB_EVENT_KEY_UP, static_cast<Ship::KbScancode>(scancode));
}

bool Fast3dWindow::KeyDown(int32_t scancode) {
    bool isProcessed = Ship::Context::GetInstance()->GetControlDeck()->ProcessKeyboardEvent(
        Ship::KbEventType::LUS_KB_EVENT_KEY_DOWN, static_cast<Ship::KbScancode>(scancode));
    Ship::Context::GetInstance()->GetWindow()->SetLastScancode(scancode);

    return isProcessed;
}

void Fast3dWindow::AllKeysUp() {
    Ship::Context::GetInstance()->GetControlDeck()->ProcessKeyboardEvent(Ship::KbEventType::LUS_KB_EVENT_ALL_KEYS_UP,
                                                                         Ship::KbScancode::LUS_KB_UNKNOWN);
}

bool Fast3dWindow::MouseButtonUp(int button) {
    return Ship::Context::GetInstance()->GetControlDeck()->ProcessMouseButtonEvent(false,
                                                                                   static_cast<Ship::MouseBtn>(button));
}

bool Fast3dWindow::MouseButtonDown(int button) {
    bool isProcessed = Ship::Context::GetInstance()->GetControlDeck()->ProcessMouseButtonEvent(
        true, static_cast<Ship::MouseBtn>(button));
    return isProcessed;
}

void Fast3dWindow::OnFullscreenChanged(bool isNowFullscreen) {
    std::shared_ptr<Window> wnd = Ship::Context::GetInstance()->GetWindow();

    // Re-save fullscreen enabled after
    Ship::Context::GetInstance()->GetConfig()->SetBool("Window.Fullscreen.Enabled", isNowFullscreen);
}

std::weak_ptr<Interpreter> Fast3dWindow::GetInterpreterWeak() const {
    return mInterpreter;
}

#ifdef __SWITCH__
#include <switch.h>

void Fast3dWindow::RenderThreadLoop() {
    // Pin render thread to Core 1.
    // svcSetThreadCoreMask(handle, ideal_core, affinity_mask):
    //   - CUR_THREAD_HANDLE: this thread
    //   - 1: preferred core index (Core 1)
    //   - (1U << 1): bitmask allowing only Core 1
    Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, 1, (1U << 1));
    if (R_FAILED(rc)) {
        // Fallback: allow any core except Core 0 (main thread)
        static const u64 NON_CORE0_MASK = (1U << 1) | (1U << 2) | (1U << 3);
        svcSetThreadCoreMask(CUR_THREAD_HANDLE, -1, NON_CORE0_MASK);
    }

    // Acquire GL context on this thread
    mWindowManagerApi->MakeContextCurrent();

    // Cache the Gui pointer once — it doesn't change during the render thread's lifetime.
    // This avoids shared_ptr refcount ops (atomic inc/dec) per sub-frame.
    // Gui is guaranteed non-null here: it's created in Window::Init() before any rendering.
    auto gui = Ship::Context::GetInstance()->GetWindow()->GetGui();

    std::unique_lock<std::mutex> lock(mRenderMutex);
    while (mRenderThreadRunning) {
        // Wait for work
        while (!mRenderHasWork && mRenderThreadRunning) {
            mRenderCV.wait(lock);
        }
        if (!mRenderThreadRunning) {
            break;
        }

        // Capture work parameters
        Gfx* commands = mRenderCommands;
        std::unordered_map<Mtx*, MtxF> mtxReplacements = std::move(mRenderMtxReplacements);
        bool renderImGui = mRenderImGui;
        lock.unlock();

        // Execute the render pipeline on Core 1 (with GL context).
        // Full ImGui (StartDraw/EndDraw) only runs on the first sub-frame.
        // Non-first sub-frames replay the cached ImGui draw data from the first
        // sub-frame's ImGui::Render() call — this is a cheap GL-only operation
        // (~1ms vs ~5ms) that avoids widget recomputation while preventing
        // flicker. The draw data persists until the next ImGui::NewFrame().
        if (renderImGui) {
            gui->StartDraw();
        }
        mInterpreter->StartFrame();
        mInterpreter->Run(commands, mtxReplacements);
        if (renderImGui) {
            gui->EndDraw();
        } else {
            // Replay previous ImGui draw data (overlay only, no computation).
            // ImGui::GetDrawData() is valid after Render() until next NewFrame().
#ifdef ENABLE_OPENGL
            ImDrawData* drawData = ImGui::GetDrawData();
            if (drawData) {
                ImGui_ImplOpenGL3_RenderDrawData(drawData);
            }
#endif
        }

        // Signal that GL commands are done BEFORE the buffer swap.
        // Core 0 can proceed here and start game logic while the render thread
        // may still be blocked in the swap. On Switch, the SDL backend forces
        // vsync on and SDL_GL_SwapWindow can block for the vsync interval.
        mGlCommandsDone.store(true, std::memory_order_release);

        // This calls SwapBuffersBegin → SDL_GL_SwapWindow. On Switch (SDL
        // backend) vsync is forced on, so SDL_GL_SwapWindow can block for the
        // duration of the vsync interval instead of returning quickly.
        mInterpreter->EndFrame();

        lock.lock();
        mRenderHasWork = false;
        mRenderWorkDone = true;
        mRenderDoneCV.notify_one();
    }

    // Release GL context before thread exits
    mWindowManagerApi->ReleaseContext();
}

void Fast3dWindow::InitRenderThread() {
    {
        std::unique_lock<std::mutex> lock(mRenderMutex);
        if (mRenderThreadRunning.load()) {
            return;
        }
        mRenderThreadRunning.store(true);
        mRenderHasWork = false;
        mRenderWorkDone = true;
        mGlCommandsDone.store(true, std::memory_order_relaxed);
    }

    // Release GL context from main thread so the render thread can acquire it
    mWindowManagerApi->ReleaseContext();

    mRenderThread = std::thread(&Fast3dWindow::RenderThreadLoop, this);
}

void Fast3dWindow::DestroyRenderThread() {
    {
        std::unique_lock<std::mutex> lock(mRenderMutex);
        if (!mRenderThreadRunning.load()) {
            return;
        }
        mRenderThreadRunning.store(false);
    }
    mRenderCV.notify_all();
    if (mRenderThread.joinable()) {
        mRenderThread.join();
    }

    // Re-acquire GL context on main thread
    mWindowManagerApi->MakeContextCurrent();
}

bool Fast3dWindow::SubmitRenderWork(Gfx* commands, std::unordered_map<Mtx*, MtxF> mtxReplacements,
                                    bool renderImGui) {
    {
        std::unique_lock<std::mutex> lock(mRenderMutex);
        if (!mRenderThreadRunning) {
            // Fallback: run inline if render thread not active
            lock.unlock();
            return DrawAndRunGraphicsCommands(commands, mtxReplacements);
        }
        // Wait for any previous work to complete
        while (!mRenderWorkDone) {
            mRenderDoneCV.wait(lock);
        }
        mRenderCommands = commands;
        mRenderMtxReplacements = std::move(mtxReplacements);
        mRenderImGui = renderImGui;
        mRenderHasWork = true;
        mRenderWorkDone = false;
        mGlCommandsDone.store(false, std::memory_order_relaxed);
    }
    mRenderCV.notify_one();
    return true;
}

void Fast3dWindow::WaitForRenderDone() {
    std::unique_lock<std::mutex> lock(mRenderMutex);
    while (!mRenderWorkDone) {
        mRenderDoneCV.wait(lock);
    }
}

void Fast3dWindow::WaitForGlCommandsDone() {
    // Spin-wait on atomic flag — avoids mutex+CV kernel overhead.
    // The wait is short (~10ms while GL commands run on Core 1),
    // and Core 0 needs to proceed immediately after (game logic).
    // Also break if the render thread has stopped to avoid hanging.
    while (!mGlCommandsDone.load(std::memory_order_acquire)) {
        if (!mRenderThreadRunning) {
            break;
        }
        std::this_thread::yield();
    }
}
#endif

} // namespace Fast
