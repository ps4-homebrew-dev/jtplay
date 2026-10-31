// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <RmlUi/Core.h>

#include "RmlUi/RmlUi_Backend.h"
#include "app.h"

#define SCREEN_WIDTH  1920
#define SCREEN_HEIGHT 1080

extern "C" {
  int sceSystemServiceLoadExec(const char* path, char* const argv[]);
}


static void PrintUsage(const char* argv0) {
  std::printf(
    "usage: %s [options]\n"
    "\n"
    "  -a, --assets DIR   asset directory containing main.rml\n"
    "                     (default: ./assets)\n"
    "  -f, --fonts DIR    font directory\n"
    "                     (default: <assets>/../fonts)\n"
    "  -p, --plugins DIR  JavaScript plugin directory\n"
    "                     (default: <assets>/../plugins)\n"
    "  -c, --cache DIR    artwork cache directory\n"
    "                     (default: $XDG_CACHE_HOME/jtplay,\n"
    "                      i.e. ~/.cache/jtplay)\n"
    "  -h, --help         show this help\n",
    argv0);
}


extern "C" int SDL_main(int argc, char* argv[]) {
  App::Options options;

#if defined(__SCE__)
  options.assets_dir = "/app0/assets";
  options.plugins_dir = "/app0/plugins";
  options.fonts_dir = "/app0/fonts";
  options.cache_dir = ::getenv("XDG_CACHE_HOME");
  ::setenv("SSL_CERT_FILE", "/app0/ca-bundle.crt", 0);
#endif

  for (int i = 1; i < argc; i++) {
    auto arg = [&](const char* s, const char* l) {
      return !std::strcmp(argv[i], s) || !std::strcmp(argv[i], l);
    };
    auto value = [&]() -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s: missing argument for %s\n", argv[0], argv[i]);
        std::exit(2);
      }
      return argv[++i];
    };

    if (arg("-a", "--assets")) options.assets_dir = value();
    else if (arg("-f", "--fonts")) options.fonts_dir = value();
    else if (arg("-p", "--plugins")) options.plugins_dir = value();
    else if (arg("-c", "--cache")) options.cache_dir = value();
    else if (arg("-h", "--help")) { PrintUsage(argv[0]); return 0; }
    else {
      std::fprintf(stderr, "%s: unknown option '%s'\n", argv[0], argv[i]);
      PrintUsage(argv[0]);
      return 2;
    }
  }

  // Trailing-slash tolerance, then derive the font directory: a sibling of
  // the asset directory, matching the repository layout.
  while (options.assets_dir.size() > 1 && options.assets_dir.back() == '/')
    options.assets_dir.pop_back();

  if (options.fonts_dir.empty())
    options.fonts_dir = options.assets_dir + "/../fonts";

  if (options.plugins_dir.empty())
    options.plugins_dir = options.assets_dir + "/../plugins";

  if (!Backend::Initialize("jtplay", SCREEN_WIDTH, SCREEN_HEIGHT, false))
    return -1;

  Rml::SetSystemInterface(Backend::GetSystemInterface());
  Rml::SetRenderInterface(Backend::GetRenderInterface());
  Rml::Initialise();

  auto fail = [](const std::string& message) {
    Rml::Log::Message(Rml::Log::LT_ERROR, "%s", message.c_str());
    Rml::Shutdown();
    Backend::Shutdown();
    return -1;
  };

  Rml::Context* ctx = Rml::CreateContext("main", Rml::Vector2i(SCREEN_WIDTH, SCREEN_HEIGHT));
  if (!ctx)
    return fail("Failed to create main context");

  bool fonts_ok = true;
  for (const char* face : {"LatoLatin-Bold.ttf", "LatoLatin-BoldItalic.ttf",
                           "LatoLatin-Italic.ttf", "LatoLatin-Regular.ttf"})
    fonts_ok &= Rml::LoadFontFace(options.fonts_dir + "/" + face, false);
  fonts_ok &= Rml::LoadFontFace(options.fonts_dir + "/NotoEmoji-VariableFont_wght.ttf", true);
  if (!fonts_ok)
    return fail("Failed to load fonts from '" + options.fonts_dir + "' (see --fonts)");

  App app;
  std::string err;
  if (!app.Initialize(ctx, options, err))
    return fail("Failed to initialize app: " + err);

  while (Backend::ProcessEvents(ctx)) {
    app.Update();
    ctx->Update();

    // The video goes straight to the backbuffer; the document, whose body
    // is transparent while watching, is composited over it.
    Backend::BeginFrame([&app](int width, int height) { app.RenderVideo(width, height); });
    ctx->Render();
    Backend::PresentFrame();
  }

  app.Shutdown();
  Rml::Shutdown();
  Backend::Shutdown();

  return 0;
}

#if defined(__linux__)
int main(int argc, char* argv[]) {
  return SDL_main(argc, argv);
}
#endif
