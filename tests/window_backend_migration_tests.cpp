#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

#include "fast/Fast3dWindow.h"
#include "ship/Context.h"
#include "ship/config/Config.h"

// ============================================================
// GetSavedWindowBackend config migration
//
// Backend IDs were renumbered once already (DX11 0→1, OpenGL 1→2,
// Metal 2→3), which silently moved existing configs onto a different
// renderer. These tests cover the recovery path: the saved backend
// name identifies the renderer even when the saved ID has gone stale.
//
// Uses a real Context + Fast3dWindow (constructed, never Init()ed, so
// no graphics/window work happens) against a config in a temp dir.
// ============================================================

namespace {

// GetSavedWindowBackend is protected; surface it for assertions.
class TestFast3dWindow final : public Fast::Fast3dWindow {
  public:
    using Fast::Fast3dWindow::GetSavedWindowBackend;
    using Fast::Fast3dWindow::SetWindowBackend;
};

class WindowBackendMigrationTest : public ::testing::Test {
  protected:
    // Context and window are created once and the window is intentionally
    // leaked: Fast3dWindow teardown assumes a fully Init()ed window (Gui
    // shuts down ImGui), which these tests never create. The resolve path
    // under test re-reads the config on every call, so one instance serves
    // every test.
    static void SetUpTestSuite() {
        sTempDir = std::filesystem::temp_directory_path() / "lus_backend_migration_test";
        std::filesystem::create_directories(sTempDir);
#ifdef _WIN32
        _putenv_s("SHIP_HOME", sTempDir.string().c_str());
#else
        setenv("SHIP_HOME", sTempDir.string().c_str(), 1);
#endif
        Ship::Context::CreateUninitializedInstance("lus tests", "lustests", "backend_migration.cfg.json");
        Ship::Context::GetRawInstance()->InitConfiguration();
        Ship::Context::GetRawInstance()->InitConsoleVariables();
        Ship::Context::GetRawInstance()->InitConsole();
        sWindow = new TestFast3dWindow();
    }

    // Destroy the context while spdlog's statics are still alive; letting it
    // ride to static destruction crashes in logger flush.
    static void TearDownTestSuite() {
        Ship::Context::DestroyInstance();
    }

    void SetUp() override {
        mConfig = Ship::Context::GetRawInstance()->GetConfig();
        mConfig->Erase("Window.Backend.Id");
        mConfig->Erase("Window.Backend.Name");
    }

    static std::filesystem::path sTempDir;
    static TestFast3dWindow* sWindow;
    std::shared_ptr<Ship::Config> mConfig;
};

std::filesystem::path WindowBackendMigrationTest::sTempDir;
TestFast3dWindow* WindowBackendMigrationTest::sWindow = nullptr;

} // anonymous namespace

// A stale ID with a valid saved name must resolve by name, and the stored
// ID must be healed to match. OpenGL is registered on every platform, so
// this covers the migration path everywhere CI runs.
TEST_F(WindowBackendMigrationTest, SavedNameRecoversStaleId) {
    // Pre-renumbering OpenGL config: id 1 now names DX11 (or nothing).
    mConfig->SetInt("Window.Backend.Id", 1);
    mConfig->SetString("Window.Backend.Name", "OpenGL");

    EXPECT_EQ(sWindow->GetSavedWindowBackend(), Fast::WindowBackend::FAST3D_SDL_OPENGL);
    EXPECT_EQ(mConfig->GetInt("Window.Backend.Id", -1), Fast::WindowBackend::FAST3D_SDL_OPENGL);
}

TEST_F(WindowBackendMigrationTest, MatchingNameAndIdResolveUnchanged) {
    mConfig->SetInt("Window.Backend.Id", Fast::WindowBackend::FAST3D_SDL_OPENGL);
    mConfig->SetString("Window.Backend.Name", "OpenGL");

    EXPECT_EQ(sWindow->GetSavedWindowBackend(), Fast::WindowBackend::FAST3D_SDL_OPENGL);
    EXPECT_EQ(mConfig->GetInt("Window.Backend.Id", -1), Fast::WindowBackend::FAST3D_SDL_OPENGL);
}

// An unrecognized name must not disturb the existing ID validation path.
TEST_F(WindowBackendMigrationTest, UnknownNameFallsThroughToSavedId) {
    mConfig->SetInt("Window.Backend.Id", Fast::WindowBackend::FAST3D_SDL_OPENGL);
    mConfig->SetString("Window.Backend.Name", "Glide64");

    EXPECT_EQ(sWindow->GetSavedWindowBackend(), Fast::WindowBackend::FAST3D_SDL_OPENGL);
}

// The case from the original report: a pre-renumbering Metal config
// (id 2) re-resolved as OpenGL, switching macOS users off Metal.
TEST_F(WindowBackendMigrationTest, PreRenumberingMetalConfigStaysOnMetal) {
    if (!sWindow->IsAvailableWindowBackend(Fast::WindowBackend::FAST3D_SDL_METAL)) {
        GTEST_SKIP() << "Metal not available on this platform";
    }

    mConfig->SetInt("Window.Backend.Id", 2); // pre-renumbering Metal; now OpenGL's ID
    mConfig->SetString("Window.Backend.Name", "Metal");

    EXPECT_EQ(sWindow->GetSavedWindowBackend(), Fast::WindowBackend::FAST3D_SDL_METAL);
    EXPECT_EQ(mConfig->GetInt("Window.Backend.Id", -1), Fast::WindowBackend::FAST3D_SDL_METAL);
}

// Startup resolution must never rewrite the saved choice; only an explicit
// (persisting) backend change may. A resolve-and-rewrite on launch is what
// let the renumbering bug permanently overwrite saved configs.
TEST_F(WindowBackendMigrationTest, NonPersistingSetLeavesConfigUntouched) {
    sWindow->SetWindowBackend(Fast::WindowBackend::FAST3D_SDL_OPENGL, false);
    EXPECT_EQ(mConfig->GetInt("Window.Backend.Id", -1), -1);
    EXPECT_EQ(mConfig->GetString("Window.Backend.Name", ""), "");

    sWindow->SetWindowBackend(Fast::WindowBackend::FAST3D_SDL_OPENGL);
    EXPECT_EQ(mConfig->GetInt("Window.Backend.Id", -1), Fast::WindowBackend::FAST3D_SDL_OPENGL);
    EXPECT_EQ(mConfig->GetString("Window.Backend.Name", ""), "OpenGL");
}
