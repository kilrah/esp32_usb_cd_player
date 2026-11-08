# Generates the display text fonts main\fonts\font_text_14.c and font_text_20.c (LVGL format) with lv_font_conv:
# Montserrat Medium (ASCII, Latin-1 Supplement, Latin Extended-A, typographic punctuation) + the LVGL built-in
# symbols (FontAwesome), from the font files that ship with LVGL. Run from the repository root:
#   .\tools\font_conv.ps1
# Needs Node.js (npx downloads lv_font_conv). Keep the ranges in sync with in_font() in main/ui.c.
# lv_font_conv 1.5.2 has no --lv-font-name / --lv-include: the font name comes from the output file name, and
# main/CMakeLists.txt defines LV_LVGL_H_INCLUDE_SIMPLE for these files.
New-Item -ItemType Directory -Force main\fonts | Out-Null
$src = "managed_components\lvgl__lvgl\scripts\built_in_font"
$range = "0x20-0x7E,0xA0-0x17F,0x2010-0x2015,0x2018-0x201E,0x2020-0x2022,0x2026,0x2030,0x2032-0x2033,0x2039-0x203A,0x20AC,0x2122"
$syms = "61441,61448,61451,61452,61453,61457,61459,61461,61465,61468,61473,61478,61479,61480,61502,61507,61512,61515,61516,61517,61521,61522,61523,61524,61543,61544,61550,61552,61553,61556,61559,61560,61561,61563,61587,61589,61636,61637,61639,61641,61664,61671,61674,61683,61724,61732,61787,61931,62016,62017,62018,62019,62020,62087,62099,62212,62189,62810,63426,63650"
foreach ($sz in 14, 20) { npx --yes lv_font_conv@1.5.2 --no-compress --no-prefilter --bpp 4 --size $sz --font "$src\Montserrat-Medium.ttf" -r $range --font "$src\FontAwesome5-Solid+Brands+Regular.woff" -r $syms --format lvgl --force-fast-kern-format -o main\fonts\font_text_$sz.c }
