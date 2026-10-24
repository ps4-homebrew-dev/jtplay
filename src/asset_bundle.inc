
#define INCASSET(name, file)                                     \
  __asm__(                                                       \
      ".section .rodata\n"                                      \
      ".global " #name "\n"                                     \
      ".global " #name "_end\n"                                 \
      ".global " #name "_size\n"                                \
      ".align 16\n"                                             \
      #name ":\n"                                               \
      ".incbin \"" #file "\"\n"                               \
      #name "_end:\n"                                           \
      #name "_size:\n"                                          \
      ".quad " #name "_end - " #name "\n"                       \
      ".previous\n"                                             \
  );                                                             \
  extern const uint8_t name[];                                   \
  extern const size_t name##_size;

INCASSET(file0, assets/css/style.css);
INCASSET(file1, assets/icons/antenna.png);
INCASSET(file2, assets/icons/audio.png);
INCASSET(file3, assets/icons/circle.png);
INCASSET(file4, assets/icons/computer.png);
INCASSET(file5, assets/icons/cross.png);
INCASSET(file6, assets/icons/disk.png);
INCASSET(file7, assets/icons/dpad.png);
INCASSET(file8, assets/icons/file.png);
INCASSET(file9, assets/icons/folder.png);
INCASSET(file10, assets/icons/l1.png);
INCASSET(file11, assets/icons/lock.png);
INCASSET(file12, assets/icons/logo.png);
INCASSET(file13, assets/icons/options.png);
INCASSET(file14, assets/icons/r1.png);
INCASSET(file15, assets/icons/radio.png);
INCASSET(file16, assets/icons/square.png);
INCASSET(file17, assets/icons/triangle.png);
INCASSET(file18, assets/icons/tv.png);
INCASSET(file19, assets/icons/user.png);
INCASSET(file20, assets/icons/video.png);
INCASSET(file21, assets/index.html);
INCASSET(file22, assets/js/app.js);
INCASSET(file23, assets/js/auth.js);
INCASSET(file24, assets/js/plugins.js);
INCASSET(file25, assets/js/providers/README.md);
INCASSET(file26, assets/js/providers/chaturbate.js);
INCASSET(file27, assets/js/providers/dlna.js);
INCASSET(file28, assets/js/providers/fs.js);
INCASSET(file29, assets/js/providers/manifest.json);
INCASSET(file30, assets/js/providers/smb.js);
INCASSET(file31, assets/js/providers/sr.js);
INCASSET(file32, assets/js/providers/svtplay.js);

__attribute__((constructor)) static void
constructor(void) {
  asset_register("/css/style.css", file0, file0_size, "text/css");
  asset_register("/icons/antenna.png", file1, file1_size, "image/png");
  asset_register("/icons/audio.png", file2, file2_size, "image/png");
  asset_register("/icons/circle.png", file3, file3_size, "image/png");
  asset_register("/icons/computer.png", file4, file4_size, "image/png");
  asset_register("/icons/cross.png", file5, file5_size, "image/png");
  asset_register("/icons/disk.png", file6, file6_size, "image/png");
  asset_register("/icons/dpad.png", file7, file7_size, "image/png");
  asset_register("/icons/file.png", file8, file8_size, "image/png");
  asset_register("/icons/folder.png", file9, file9_size, "image/png");
  asset_register("/icons/l1.png", file10, file10_size, "image/png");
  asset_register("/icons/lock.png", file11, file11_size, "image/png");
  asset_register("/icons/logo.png", file12, file12_size, "image/png");
  asset_register("/icons/options.png", file13, file13_size, "image/png");
  asset_register("/icons/r1.png", file14, file14_size, "image/png");
  asset_register("/icons/radio.png", file15, file15_size, "image/png");
  asset_register("/icons/square.png", file16, file16_size, "image/png");
  asset_register("/icons/triangle.png", file17, file17_size, "image/png");
  asset_register("/icons/tv.png", file18, file18_size, "image/png");
  asset_register("/icons/user.png", file19, file19_size, "image/png");
  asset_register("/icons/video.png", file20, file20_size, "image/png");
  asset_register("/index.html", file21, file21_size, "text/html");
  asset_register("/js/app.js", file22, file22_size, "text/javascript");
  asset_register("/js/auth.js", file23, file23_size, "text/javascript");
  asset_register("/js/plugins.js", file24, file24_size, "text/javascript");
  asset_register("/js/providers/README.md", file25, file25_size, "text/markdown");
  asset_register("/js/providers/chaturbate.js", file26, file26_size, "text/javascript");
  asset_register("/js/providers/dlna.js", file27, file27_size, "text/javascript");
  asset_register("/js/providers/fs.js", file28, file28_size, "text/javascript");
  asset_register("/js/providers/manifest.json", file29, file29_size, "application/json");
  asset_register("/js/providers/smb.js", file30, file30_size, "text/javascript");
  asset_register("/js/providers/sr.js", file31, file31_size, "text/javascript");
  asset_register("/js/providers/svtplay.js", file32, file32_size, "text/javascript");
}

