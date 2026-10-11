
#pragma once

#include "dantes_inferno_app.h"
#include "renderer_config.h"

class DantesInfernoNativeApp : public DantesInfernoApp {
 public:
  using DantesInfernoApp::DantesInfernoApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<DantesInfernoNativeApp>(
        new DantesInfernoNativeApp(ctx, "dantes_inferno", PPCImageConfig));
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    DantesInfernoApp::OnPreSetup(config);
    renderer::ApplyNativeRenderScale();
    if (auto graphics = renderer::CreateGraphicsSystem()) {
      config.graphics = std::move(graphics);
    }
  }
};
