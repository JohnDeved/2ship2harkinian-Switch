#pragma once
#include "ship/window/Window.h"
#include "ship/window/gui/Gui.h"
#include "ship/controller/controldevice/controller/mapping/keyboard/KeyboardScancodes.h"

union Gfx;
#include "interpreter.h"

#ifdef __SWITCH__
#include <mutex>
#include <condition_variable>
#include <thread>
#endif

namespace Fast {
class Fast3dWindow : public Ship::Window {
  public:
    Fast3dWindow();
    Fast3dWindow(std::vector<std::shared_ptr<Ship::GuiWindow>> guiWindows);
    Fast3dWindow(std::shared_ptr<Ship::Gui> gui);
    ~Fast3dWindow();

    void Init() override;
    void Close() override;
    void RunGuiOnly() override;
    void StartFrame() override;
    void EndFrame() override;
    bool IsFrameReady() override;
    void HandleEvents() override;
    void SetCursorVisibility(bool visible) override;
    uint32_t GetWidth() override;
    uint32_t GetHeight() override;
    int32_t GetPosX() override;
    int32_t GetPosY() override;
    float GetAspectRatio() override;
    void SetMousePos(Ship::Coords pos) override;
    Ship::Coords GetMousePos() override;
    Ship::Coords GetMouseDelta() override;
    Ship::CoordsF GetMouseWheel() override;
    bool GetMouseState(Ship::MouseBtn btn) override;
    void SetMouseCapture(bool capture) override;
    bool IsMouseCaptured() override;
    uint32_t GetCurrentRefreshRate() override;
    bool SupportsWindowedFullscreen() override;
    bool CanDisableVerticalSync() override;
    void SetResolutionMultiplier(float multiplier) override;
    void SetMsaaLevel(uint32_t value) override;
    void SetFullscreen(bool isFullscreen) override;
    bool IsFullscreen() override;
    bool IsRunning() override;
    uintptr_t GetGfxFrameBuffer() override;
    const char* GetKeyName(int32_t scancode) override;

    void InitWindowManager();
    int32_t GetTargetFps();
    void SetTargetFps(int32_t fps);
    void SetMaximumFrameLatency(int32_t latency);
    void GetPixelDepthPrepare(float x, float y);
    uint16_t GetPixelDepth(float x, float y);
    void SetTextureFilter(FilteringMode filteringMode);
    void SetRendererUCode(UcodeHandlers ucode);
    void EnableSRGBMode();
    bool DrawAndRunGraphicsCommands(Gfx* commands, const std::unordered_map<Mtx*, MtxF>& mtxReplacements);
    const Fast3DStats& GetFrameStats() const;
    void ResetFrameStats();
    void SetProfilingEnabled(bool enabled);

    std::weak_ptr<Interpreter> GetInterpreterWeak() const;

#ifdef __SWITCH__
    // B1: Render thread — moves GL context + DL interpretation to Core 1.
    // Core 0 submits render work, Core 1 executes it with the GL context.
    void InitRenderThread();
    void DestroyRenderThread();
    bool SubmitRenderWork(Gfx* commands, std::unordered_map<Mtx*, MtxF> mtxReplacements);
    void WaitForRenderDone();
    bool IsRenderThreadActive() const { return mRenderThreadRunning; }
#endif

  protected:
    static bool KeyDown(int32_t scancode);
    static bool KeyUp(int32_t scancode);
    static void AllKeysUp();
    static bool MouseButtonDown(int button);
    static bool MouseButtonUp(int button);
    static void OnFullscreenChanged(bool isNowFullscreen);

  private:
    GfxRenderingAPI* mRenderingApi;
    GfxWindowBackend* mWindowManagerApi;
    std::shared_ptr<Interpreter> mInterpreter = nullptr;

#ifdef __SWITCH__
    // B1: Render thread state
    std::thread mRenderThread;
    std::mutex mRenderMutex;
    std::condition_variable mRenderCV;
    std::condition_variable mRenderDoneCV;
    bool mRenderThreadRunning = false;
    bool mRenderHasWork = false;
    bool mRenderWorkDone = true;
    // Render work parameters (set by Core 0, read by Core 1)
    Gfx* mRenderCommands = nullptr;
    std::unordered_map<Mtx*, MtxF> mRenderMtxReplacements;

    void RenderThreadLoop();
#endif
};
} // namespace Fast
