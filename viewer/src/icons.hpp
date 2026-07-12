// Font Awesome 6 Free (Solid) glyphs used by the viewer UI.
// The font file lives in assets/fonts/fa-solid-900.ttf (SIL OFL 1.1) and is
// merged into the base font by load_fonts(); CMake copies it next to the exe.
//
// Each macro is the UTF-8 encoding of the glyph's private-use codepoint, so it
// can be concatenated straight into a button/label string, e.g.
//   ImGui::Button(ICON_FA_PLUG "  Attach")
#pragma once

// Tight glyph set (codepoint pairs) so the atlas only rasterizes what we use.
#define ICON_FA_SEARCH   "\xef\x80\x82"  // U+F002 magnifying-glass
#define ICON_FA_XMARK    "\xef\x80\x8d"  // U+F00D
#define ICON_FA_ARROW_L  "\xef\x81\xa0"  // U+F060 arrow-left  (nav back)
#define ICON_FA_ARROW_R  "\xef\x81\xa1"  // U+F061 arrow-right (nav forward)
#define ICON_FA_EXPAND   "\xef\x81\xa5"  // U+F065 expand   (maximize)
#define ICON_FA_COMPRESS "\xef\x81\xa6"  // U+F066 compress (restore)
#define ICON_FA_MINUS    "\xef\x81\xa8"  // U+F068
#define ICON_FA_PLUG     "\xef\x87\xa6"  // U+F1E6
#define ICON_FA_CIRCLE_Q "\xef\x81\x99"  // U+F059 circle-question
