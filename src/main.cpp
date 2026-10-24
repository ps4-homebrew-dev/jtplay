// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <string>

#include <RmlUi/Core.h>

#include "RmlUi/RmlUi_Backend.h"
#include "app.h"

#define SCREEN_WIDTH  1920
#define SCREEN_HEIGHT 1080


typedef struct SceAppContentInitParam {
  char reserved[32];
} SceAppContentInitParam;

typedef struct SceAppContentBootParam {
  char     reserved1[4];
  uint32_t attr;
  char     reserved2[32];
} SceAppContentBootParam;

typedef struct SceAppContentMountPoint {
  char data[16];
} SceAppContentMountPoint;


extern "C" {
  int sceSystemServiceLoadExec(const char* path, char* const argv[]);
  int sceAppContentInitialize(const SceAppContentInitParam *initParam,
			      SceAppContentBootParam *bootParam);
  int sceAppContentTemporaryDataMount2(uint32_t option, SceAppContentMountPoint *mountPoint);
  int sceAppContentTemporaryDataUnmount(const SceAppContentMountPoint *mountPoint);
  int sceSysmoduleLoadModule(uint64_t);
}

#if defined(__ORBIS__)
static SceAppContentMountPoint  g_tmpmnt = {0};

__attribute__((constructor))
static void OrbisInit(void) {
  SceAppContentInitParam ip = {};
  SceAppContentBootParam bp = {};

  sceSysmoduleLoadModule(0x00b4);
  sceAppContentInitialize(&ip, &bp);
  sceAppContentTemporaryDataMount2(0, &g_tmpmnt);
}
#endif


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

static int Run(int argc, char* argv[]) {
  App::Options options;
  std::string fonts_dir;

#if defined(__ORBIS__)
  options.assets_dir = "/app0/assets";
  options.plugins_dir = "/app0/plugins";
  options.cache_dir = "/temp0";
  fonts_dir = "/app0/fonts";
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

    if (arg("-a", "--assets"))     options.assets_dir = value();
    else if (arg("-f", "--fonts")) fonts_dir = value();
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
  if (fonts_dir.empty())
    fonts_dir = options.assets_dir + "/../fonts";
  if (options.plugins_dir.empty())
    options.plugins_dir = options.assets_dir + "/../plugins";

  Rml::Context* ctx;
  std::string err;
  App app;

  if (!Backend::Initialize("jtplay", SCREEN_WIDTH, SCREEN_HEIGHT, false)) {
    return -1;
  }

  Rml::SetSystemInterface(Backend::GetSystemInterface());
  Rml::SetRenderInterface(Backend::GetRenderInterface());
  Rml::Initialise();

  if (!(ctx = Rml::CreateContext("main", Rml::Vector2i(SCREEN_WIDTH, SCREEN_HEIGHT)))) {
    Rml::Log::Message(Rml::Log::LT_ERROR, "Failed to create main context");
    Rml::Shutdown();
    Backend::Shutdown();
    return -1;
  }

  bool fonts_ok = true;
  for (const char* face : {"LatoLatin-Bold.ttf", "LatoLatin-BoldItalic.ttf",
                           "LatoLatin-Italic.ttf", "LatoLatin-Regular.ttf"})
    fonts_ok &= Rml::LoadFontFace(fonts_dir + "/" + face, false);
  fonts_ok &= Rml::LoadFontFace(fonts_dir + "/NotoEmoji-VariableFont_wght.ttf", true);
  if (!fonts_ok) {
    Rml::Log::Message(Rml::Log::LT_ERROR,
      "Failed to load fonts from '%s' (see --fonts)", fonts_dir.c_str());
    Rml::Shutdown();
    Backend::Shutdown();
    return -1;
  }

  if (!app.Initialize(ctx, options, err)) {
    Rml::Log::Message(Rml::Log::LT_ERROR, "Failed to initialize app: %s", err.c_str());
    Rml::Shutdown();
    Backend::Shutdown();
    return -1;
  }

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

int main(int argc, char* argv[]) {
  const int status = Run(argc, argv);
#if defined(__ORBIS__)
  // A PS4 process that returns from main() is reported as a crash; asking
  // the system to close the title is the clean way out.
  sceSystemServiceLoadExec("exit", nullptr);
#endif
  return status;
}
