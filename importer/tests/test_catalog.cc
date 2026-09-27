// catalog-win.json generation (catalog.h).
//
//   test_import_catalog unit <scratch>
//     the text and record readers against hand-made inputs, whole synthetic
//     PE32 / NE modules (tests/module_builder.h), a scan of a synthetic FILES
//     tree, the JSON layout, and regenerate_catalog (adimport --catalog-only)
//   test_import_catalog real <scratch> <adimport.exe> <reference.json>
//     semantic identity with the prototype's output (research/win/
//     make_catalog.py -> research/win/catalog-win.json) over the real
//     corpus, both through the library and through adimport --catalog-only
//     on a scratch copy of the modules. Exit 77 (skipped) when the imported
//     assets or the reference are not on this machine.
#include <phosg/JSON.hh>

#include <functional>
#include <set>

#include "catalog.h"
#include "importer.h"
#include "module_builder.h"
#include "run_process.h"
#include "test_util.h"

using namespace adw::import;
namespace fs = std::filesystem;

namespace {

std::string bytes(std::string_view s) { return std::string(s); }

// ---- text -------------------------------------------------------------------------

void test_text() {
  CHECK_EQ(cp1252_to_utf8("A\x80\x81\xA9\xFF\x9D"), std::string("A\xE2\x82\xAC\xEF\xBF\xBD\xC2\xA9\xC3\xBF\xEF\xBF\xBD"));
  CHECK_EQ(cp1252_to_utf8("\x93quoted\x94 \x96 \x85"),
           std::string("\xE2\x80\x9Cquoted\xE2\x80\x9D \xE2\x80\x93 \xE2\x80\xA6"));
  CHECK_EQ(std::string(c_string(bytes(std::string_view("ab\0cd", 5)))), std::string("ab"));
  CHECK_EQ(std::string(c_string("abc")), std::string("abc"));

  auto rtf = [](std::string_view s) { return rtf_to_text(s); };
  // Tables are not text; paragraphs, tabs and \'xx are; the NUL after the
  // closing brace (and anything past it) is not read.
  CHECK_EQ(rtf(std::string("{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0\\fswiss Arial;}}{\\colortbl;\\red0\\green0\\blue0;}\r\n"
                           "\\pard\\plain\\f0\\fs20 Hello\\par World\\tab x\\'a9 1996\\par\\par\\par\\par End}") +
               std::string(1, '\0') + "{junk}"),
           std::string("Hello\nWorld\tx\xC2\xA9 1996\n\nEnd"));
  CHECK_EQ(rtf(std::string("{\\rtf1 A") + std::string(1, '\0') + "B}"), std::string("A"));
  CHECK_EQ(rtf("{\\rtf1 A{\\*\\generator Foo 1.0;}B}"), std::string("AB"));
  CHECK_EQ(rtf("{\\rtf1 A{\\pict\\wmetafile8 0123abcd}B{\\info{\\author me}}C}"), std::string("ABC"));
  CHECK_EQ(rtf("{\\rtf1 \\\\ \\{ \\} a\\~b}"), std::string("\\ { } a b"));
  // A control word eats one following space and its numeric parameter.
  CHECK_EQ(rtf("{\\rtf1\\li-360 X\\fi720  Y}"), std::string("X Y"));
  // Blanks before a line break go; runs of 3+ breaks become 2 — after the
  // blanks went, so "\n\t\n\n" is a run of three.
  CHECK_EQ(rtf("{\\rtf1 A  \\par B}"), std::string("A\nB"));
  CHECK_EQ(rtf("{\\rtf1 A\\par\\tab\\par\\par B}"), std::string("A\n\nB"));
  CHECK_EQ(rtf("{\\rtf1 A\\line\\line B}"), std::string("A\n\nB"));
  // Only lower-case control words are words: "\Foo" is the symbol \F + "oo".
  CHECK_EQ(rtf("{\\rtf1 \\Foo x}"), std::string("oo x"));
  // A backslash before a raw line break (or at the very end) is dropped.
  CHECK_EQ(rtf("{\\rtf1 A\\\nB\\"), std::string("AB"));
  // Raw CR/LF in the source are not text.
  CHECK_EQ(rtf("{\\rtf1 A\r\nB}"), std::string("AB"));
  // The outermost group ends the document; a stray '}' ends it too.
  CHECK_EQ(rtf("{\\rtf1 A}B"), std::string("A"));
  CHECK_EQ(rtf("text} more"), std::string("text"));
  CHECK_EQ(rtf("no braces at all"), std::string("no braces at all"));
  // Trimming covers NO-BREAK SPACE (str.strip does); undefined bytes decode
  // to U+FFFD.
  CHECK_EQ(rtf("{\\rtf1 \\'a0 X \\'a0}"), std::string("X"));
  CHECK_EQ(rtf("{\\rtf1 \\'81\\'zz}"), std::string("\xEF\xBF\xBDzz"));
  CHECK_EQ(rtf(""), std::string());

  std::string sl = test::stringlist({"First", "Caf\xE9", "x"});
  auto items = parse_stringlist(sl);
  CHECK(items.size() == 3 && items[0] == "First" && items[1] == "Caf\xC3\xA9" && items[2] == "x");
  CHECK(parse_stringlist("").empty());
  CHECK(parse_stringlist("\x01").empty());
  items = parse_stringlist(std::string("\x03\x00" "a\0bc", 6));   // count says 3, one unterminated
  CHECK(items.size() == 2 && items[0] == "a" && items[1] == "bc");
}

// ---- control records ------------------------------------------------------------------

void test_records() {
  // Kinds 0 and unknown are not controls; nor is a record without a kind.
  CHECK(!parse_control_record(test::record_head(0, "None", 0, 0), 0));
  CHECK(!parse_control_record(test::record_head(9, "Nine", 0, 0), 0));
  CHECK(!parse_control_record("\x05", 0));

  auto cb = parse_control_record(test::checkbox_record("Clear Screen First", 3), 2);
  CHECK(cb && cb->index == 2 && cb->name == "Clear Screen First" && cb->kind == "checkbox" && cb->type == "checkbox" &&
        cb->def == 1);
  CHECK(parse_control_record(test::checkbox_record("Off", 0), 0)->def == 0);
  auto bt = parse_control_record(test::button_record("Pictures"), 0);
  CHECK(bt && bt->kind == "button" && bt->type == "button" && !bt->def);
  // The name field is 20 bytes of Windows-1252.
  CHECK_EQ(parse_control_record(test::checkbox_record("Caf\xE9", 0), 0)->name, std::string("Caf\xC3\xA9"));
  CHECK_EQ(parse_control_record(test::checkbox_record("ABCDEFGHIJKLMNOPQRSTUVWXYZ", 0), 0)->name,
           std::string("ABCDEFGHIJKLMNOPQRST"));
  // A record cut short after the name still has a kind and a name.
  auto shortcb = parse_control_record(test::checkbox_record("Short", 1).substr(0, 0x10), 1);
  CHECK(shortcb && shortcb->name == "Short" && shortcb->def == 0);

  // String slider, values from 0 (AD4 shape): no extra stop; the default
  // stop is the last whose value does not exceed the default.
  auto ss = parse_control_record(
      test::string_slider_record("Critic Appears:", {"Never", "Rarely", "Often", "Always"}, {0, 33, 66, 100}, 50), 1);
  CHECK(ss && ss->kind == "stringslider" && ss->type == "slider");
  CHECK((ss->items == std::vector<std::string>{"Never", "Rarely", "Often", "Always"}));
  CHECK((ss->values == std::vector<int>{0, 33, 66, 100}));
  CHECK(ss->def == 33 && ss->default_stop == 1 && !ss->bold_stop);
  ss = parse_control_record(test::string_slider_record("S", {"a", "b"}, {0, 50}, 1000), 0);
  CHECK(ss->default_stop == 1 && ss->def == 50);
  // Classic shape: stored lower bounds start above 0, so the host prepends a
  // 0 value and repeats the last label, flagged bold.
  ss = parse_control_record(
      test::string_slider_record("Objects", {"Flight", "Squadron", "Air Wing", "Swarm"}, {25, 50, 75, 100}, 60), 0);
  CHECK((ss->items == std::vector<std::string>{"Flight", "Squadron", "Air Wing", "Swarm", "Swarm"}));
  CHECK((ss->values == std::vector<int>{0, 25, 50, 75, 100}));
  CHECK(ss->default_stop == 2 && ss->def == 50 && ss->bold_stop == 4);
  // A value table cut off by the record's end reads as all zeros.
  std::string cut = test::string_slider_record("Cut", {"x", "y", "z"}, {10, 20, 30}, 0);
  ss = parse_control_record(cut.substr(0, cut.size() - 1), 0);
  CHECK((ss->values == std::vector<int>{0, 0, 0}) && ss->items.size() == 3 && !ss->bold_stop && ss->default_stop == 2);
  // No stops at all: the prototype's defaultStop -1 and default 0.
  ss = parse_control_record(test::string_slider_record("Empty", {}, {}, 5), 0);
  CHECK(ss->items.empty() && ss->values.empty() && ss->def == 0 && ss->default_stop == -1);
  // The host keeps at most 101 stops; an appended stop past that is dropped
  // with its bold flag.
  std::vector<std::string> many;
  std::vector<uint16_t> many_values;
  for (int i = 0; i < 101; i++) many.push_back("s" + std::to_string(i)), many_values.push_back(uint16_t(i + 1));
  ss = parse_control_record(test::string_slider_record("Many", many, many_values, 3), 0);
  CHECK(ss->items.size() == 101 && ss->values.size() == 101 && !ss->bold_stop);
  CHECK(ss->values[0] == 0 && ss->values[100] == 100 && ss->default_stop == 3);

  // Numeric slider: the default is clamped, the raw one kept; a unit only
  // when the record has one.
  auto ns = parse_control_record(test::num_slider_record("# of Drops", 1, 9, 22), 0);
  CHECK(ns && ns->kind == "numslider" && ns->type == "slider" && ns->min == 1 && ns->max == 9 && ns->def == 9 &&
        ns->raw_default == 22 && ns->unit.empty());
  ns = parse_control_record(test::num_slider_record("Height:", -5, 95, -40, "%", 2), 0);
  CHECK(ns->def == -5 && ns->raw_default == -40 && ns->unit == "%" && ns->unit_pos == "suffix");
  CHECK(parse_control_record(test::num_slider_record("Cost", 0, 9, 3, "$", 1), 0)->unit_pos == "prefix");
  CHECK(parse_control_record(test::num_slider_record("Speed", 0, 9, 3, "x", 0), 0)->unit_pos == "none");
  ns = parse_control_record(test::record_head(2, "Bare", 0, 7), 0);   // no min/max bytes at all
  CHECK(ns->min == 0 && ns->max == 0 && ns->def == 0 && ns->raw_default == 7);

  // Popup: the default index is clamped (negative -> 0), items capped at 101.
  auto pp = parse_control_record(test::popup_record("Display:", {"Random", "In order", "Reverse"}, 5), 3);
  CHECK(pp && pp->kind == "popup" && pp->type == "popup" && pp->items.size() == 3 && pp->def == 2 && pp->index == 3);
  CHECK(parse_control_record(test::popup_record("P", {"a", "b"}, -1), 0)->def == 0);
  CHECK(parse_control_record(test::popup_record("P", {}, 0), 0)->def == -1);   // the prototype's n-1
}

// ---- synthetic modules ------------------------------------------------------------------

std::string synth_pe(bool msvc_entry = false) {
  test::PeSpec pe;
  pe.imports = {"KERNEL32.dll", "ADXPL510.DLL", "user32.dll", "KERNEL32.dll"};
  pe.exports = {msvc_entry ? "_Module@4" : "Module"};
  pe.resources = {
      {16, "", 1, 0x409, test::version_resource({{"CompanyName", "Nobody"}, {"FileDescription", "Synth Four"}})},
      // German before English in the directory (0x407 < 0x409): English wins.
      {2000, "", 40, 0x407, "{\\rtf1 Deutsch\\par}"},
      {2000, "", 40, 0x409, "{\\rtf1\\ansi{\\fonttbl{\\f0 Arial;}}English\\par about \\'a9 1996}\0"},
      {1000, "", 1, 0x407, test::checkbox_record("Bildschirm", 0)},
      {1000, "", 1, 0x409, test::checkbox_record("Clear Screen", 1)},
      // Only one language: that one.
      {1000, "", 2, 0x40C, test::popup_record("Ordre", {"Un", "Deux"}, 1)},
      // Neutral beats French.
      {1000, "", 3, 0x40C, test::num_slider_record("Vitesse", 0, 10, 5)},
      {1000, "", 3, 0, test::num_slider_record("Speed", 0, 10, 5, "%", 2)},
      // Slot 5 does not exist in the ABI: never read.
      {1000, "", 5, 0x409, test::checkbox_record("Fifth", 1)},
      {0, "STRINGLIST", 128, 0x409, test::stringlist({"Not the name"})},
  };
  return test::build_pe(pe);
}

std::string synth_ne() {
  test::NeSpec ne;
  ne.module_refs = {"KERNEL", "USER", "ADXPL300", "AD_RSRC", "GDI", "USER"};
  ne.resources = {
      {2000, "", 20, std::string("Synth Three\0junk", 16)},
      {2000, "", 30, "About \x93this\x94\r\nline 2\r\n  "},
      {2000, "", 10, "Credits\r\n"},
      {1000, "", 1, test::string_slider_record("Density", {"Low", "High"}, {50, 100}, 75)},
      {1000, "", 2, test::popup_record("Mode", {"A", "B", "C"}, 1)},
      {1000, "", 3, test::record_head(0, "Nothing", 0, 0)},
      {1000, "", 4, test::checkbox_record("Sound", 1)},
      {3000, "", 1, "RIFF....WAVE"},
      {0, "STRINGLIST", 128, test::stringlist({"Not the name either"})},
  };
  return test::build_ne(ne);
}

void test_modules(const fs::path& dir) {
  fs::create_directories(dir);
  auto write = [&](const std::string& name, const std::string& data) {
    fs::path p = dir / to_wide(name);
    test::write_bytes(p, std::vector<uint8_t>(data.begin(), data.end()));
    return p;
  };

  CatalogModule m = catalog_module(write("SYNTH4.AD", synth_pe()), "FILES/AD40/SYNTH4.AD");
  CHECK_EQ(m.id, std::string("ad40.synth4"));
  CHECK_EQ(m.lane, std::string("pe32"));
  CHECK_EQ(m.path, std::string("FILES/AD40/SYNTH4.AD"));
  CHECK_EQ(m.display_name, std::string("Synth Four"));
  CHECK_EQ(m.about, std::string("English\nabout \xC2\xA9 1996"));
  CHECK(!m.credits);
  CHECK_EQ(m.entry, std::string("Module"));
  CHECK((m.needs == std::vector<std::string>{"ADXPL510.DLL"}));
  CHECK((m.system == std::vector<std::string>{"KERNEL32.DLL", "USER32.DLL"}));
  CHECK_EQ(m.controls.size(), size_t(3));
  if (m.controls.size() == 3) {
    CHECK(m.controls[0].index == 0 && m.controls[0].name == "Clear Screen" && m.controls[0].def == 1);
    CHECK(m.controls[1].index == 1 && m.controls[1].name == "Ordre" && m.controls[1].kind == "popup");
    CHECK(m.controls[2].index == 2 && m.controls[2].name == "Speed" && m.controls[2].unit == "%");
  }
  CHECK_EQ(catalog_module(write("STARRYNI.AD", synth_pe(true)), "FILES/ENGINE/STARRYNI.AD").entry,
           std::string("_Module@4"));

  // No FileDescription: STRINGLIST 128's first string, else the file name.
  test::PeSpec bare;
  bare.resources = {{0, "STRINGLIST", 128, 0x409, test::stringlist({"From the list", "second"})}};
  CHECK_EQ(catalog_module(write("LISTED.AD", test::build_pe(bare)), "x").display_name, std::string("From the list"));
  bare.resources = {{0, "STRINGLIST", 128, 0x409, test::stringlist({""})},
                    {16, "", 1, 0x409, test::version_resource({{"FileDescription", ""}})}};
  CatalogModule unnamed = catalog_module(write("NoName.AD", test::build_pe(bare)), "x");
  CHECK_EQ(unnamed.display_name, std::string("noname"));
  CHECK_EQ(unnamed.id, std::string("ad40.noname"));
  CHECK(unnamed.about.empty() && unnamed.controls.empty() && unnamed.needs.empty() && unnamed.system.empty());

  CatalogModule c = catalog_module(write("SYNTH3.AD", synth_ne()), "FILES/CLASSIC/SYNTH3.AD");
  CHECK_EQ(c.id, std::string("classic.synth3"));
  CHECK_EQ(c.lane, std::string("ne16"));
  CHECK_EQ(c.display_name, std::string("Synth Three"));
  CHECK_EQ(c.about, std::string("About \xE2\x80\x9Cthis\xE2\x80\x9D\nline 2"));
  CHECK(c.credits && *c.credits == "Credits");
  CHECK_EQ(c.entry, std::string("MODULE"));
  // Sorted by bytes: 'X' (0x58) before '_' (0x5F).
  CHECK((c.needs == std::vector<std::string>{"ADXPL300", "AD_RSRC"}));
  CHECK((c.system == std::vector<std::string>{"GDI", "KERNEL", "USER"}));
  CHECK_EQ(c.controls.size(), size_t(3));
  if (c.controls.size() == 3) {
    const CatalogControl& s = c.controls[0];
    CHECK(s.kind == "stringslider" && (s.values == std::vector<int>{0, 50, 100}) && s.bold_stop == 2 &&
          s.default_stop == 1 && s.def == 50);
    CHECK(c.controls[1].index == 1 && c.controls[1].def == 1);
    CHECK(c.controls[2].index == 3 && c.controls[2].kind == "checkbox");   // slot 2 was kind 0
  }
  // An empty name (just the NUL) falls back like a missing one.
  test::NeSpec ne;
  ne.resources = {{2000, "", 20, std::string(1, '\0')}, {0, "STRINGLIST", 128, test::stringlist({"Listed"})}};
  CatalogModule listed = catalog_module(write("LISTED3.AD", test::build_ne(ne)), "x");
  CHECK_EQ(listed.display_name, std::string("Listed"));
  CHECK(listed.about.empty() && !listed.credits);
  ne.resources.clear();
  CHECK_EQ(catalog_module(write("PLAIN3.AD", test::build_ne(ne)), "x").display_name, std::string("plain3"));

  // Not a module: a clear error, never a crash.
  bool threw = false;
  try {
    catalog_module(write("JUNK.AD", "MZ not really an executable, just text padding it out to 64 bytes...."), "x");
  } catch (const ImportError& e) {
    threw = e.status() == Status::source_invalid;
  }
  CHECK(threw);
  threw = false;
  try {
    catalog_module(write("CUT.AD", synth_pe().substr(0, 0x100)), "x");   // cut inside the headers
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);
}

// ---- a FILES tree ------------------------------------------------------------------------

// A FILES tree with modules of both lanes, files that are not modules, a
// damaged module, and an id collision.
void write_tree(const fs::path& files) {
  auto put = [&](const std::wstring& rel, const std::string& data) {
    test::write_bytes(files / rel, std::vector<uint8_t>(data.begin(), data.end()));
  };
  put(L"AD40\\B.AD", synth_pe());
  put(L"AD40\\A.ad", synth_pe());
  put(L"AD40\\ADXPL510.DLL", synth_pe());
  put(L"AD40\\JUNK.AD", "not a module");
  put(L"AD40\\STARRYNI.AD", synth_pe());   // takes the id the ENGINE copy would have
  put(L"ENGINE\\STARRYNI.AD", synth_pe(true));
  put(L"CLASSIC\\Z.AD", synth_ne());
  put(L"CLASSIC\\M.AD", synth_ne());
  fs::create_directories(files / L"CLASSIC" / L"SUB.AD");
}

void test_scan(const fs::path& dir) {
  fs::path files = dir / L"FILES.importing-1";   // any name: the paths still say FILES/
  write_tree(files);
  std::vector<std::string> logged;
  auto mods = scan_catalog(files, [&](const std::string& s) { logged.push_back(s); });
  std::vector<std::string> ids, paths;
  for (auto& m : mods) ids.push_back(m.id), paths.push_back(m.path);
  CHECK((ids == std::vector<std::string>{"ad40.a", "ad40.b", "ad40.starryni", "classic.m", "classic.z"}));
  CHECK((paths == std::vector<std::string>{"FILES/AD40/A.ad", "FILES/AD40/B.AD", "FILES/AD40/STARRYNI.AD",
                                           "FILES/CLASSIC/M.AD", "FILES/CLASSIC/Z.AD"}));
  CHECK_EQ(logged.size(), size_t(2));
  if (logged.size() == 2) {
    CHECK(logged[0].find("FILES/AD40/JUNK.AD") != std::string::npos);
    CHECK(logged[1].find("FILES/ENGINE/STARRYNI.AD") != std::string::npos &&
          logged[1].find("duplicate id") != std::string::npos);
  }
  bool threw = false;
  try {
    scan_catalog(dir / L"nowhere");
  } catch (const ImportError& e) {
    threw = e.status() == Status::source_invalid;
  }
  CHECK(threw);
}

// ---- JSON ----------------------------------------------------------------------------------

void test_json() {
  // PACKAGES.md §6, COVERS.md §2.7: generator adimport 1.2 and the top-level packages list.
  CHECK_EQ(render_catalog_json({}),
           std::string("{\n \"version\": 1,\n \"generator\": \"adimport 1.2\",\n \"packages\": [],\n \"modules\": []\n}\n"));
  CatalogModule m;
  m.id = "ad40.x";
  m.display_name = "X \"quoted\" \\ \x01";
  m.lane = "pe32";
  m.path = "FILES/AD40/X.AD";
  m.about = "line\n\ttab \xC2\xA9";
  m.entry = "Module";
  m.needs = {"ADXPL510.DLL"};
  CatalogControl b;
  b.index = 0;
  b.name = "Pictures";
  b.kind = b.type = "button";
  m.controls.push_back(b);
  std::string j = render_catalog_json({m});
  // Python's json.dump(indent=1, ensure_ascii=False) layout and escapes.
  CHECK(j.find("\n   \"displayName\": \"X \\\"quoted\\\" \\\\ \\u0001\",\n") != std::string::npos);
  CHECK(j.find("\"about\": \"line\\n\\ttab \xC2\xA9\"") != std::string::npos);
  CHECK(j.find("   \"controls\": [\n    {\n     \"index\": 0,\n     \"name\": \"Pictures\",\n     \"kind\": \"button\",\n"
               "     \"type\": \"button\"\n    }\n   ],\n") != std::string::npos);
  CHECK(j.find("   \"needs\": [\n    \"ADXPL510.DLL\"\n   ],\n   \"system\": []\n  }\n ]\n}\n") != std::string::npos);
  CHECK(j.find("credits") == std::string::npos);

  // Every field of every kind, read back independently.
  CatalogModule c;
  c.id = "classic.y";
  c.display_name = "Y";
  c.lane = "ne16";
  c.path = "FILES/CLASSIC/Y.AD";
  c.credits = "someone";
  c.entry = "MODULE";
  c.controls.push_back(*parse_control_record(
      test::string_slider_record("S", {"a", "b"}, {10, 20}, 15), 0));
  c.controls.push_back(*parse_control_record(test::num_slider_record("N", 1, 9, 22, "%", 1), 1));
  c.controls.push_back(*parse_control_record(test::popup_record("P", {"x", "y"}, 1), 2));
  c.controls.push_back(*parse_control_record(test::checkbox_record("C", 1), 3));
  phosg::JSON doc = phosg::JSON::parse(render_catalog_json({c}));
  const phosg::JSON& y = *doc.at("modules").as_list().at(0);
  CHECK_EQ(y.get_string("credits"), std::string("someone"));
  CHECK_EQ(y.get_string("about"), std::string());
  const auto& ctl = y.at("controls").as_list();
  CHECK_EQ(ctl.size(), size_t(4));
  if (ctl.size() == 4) {
    const phosg::JSON& s = *ctl[0];
    CHECK(s.get_string("kind") == "stringslider" && s.get_string("type") == "slider");
    CHECK(s.at("items").as_list().size() == 3 && s.at("values").as_list().size() == 3);
    CHECK(s.get_int("default") == 10 && s.get_int("defaultStop") == 1 && s.get_int("boldStop") == 2);
    const phosg::JSON& n = *ctl[1];
    CHECK(n.get_int("min") == 1 && n.get_int("max") == 9 && n.get_int("default") == 9 && n.get_int("rawDefault") == 22);
    CHECK(n.get_string("unit") == "%" && n.get_string("unitPos") == "prefix");
    const phosg::JSON& p = *ctl[2];
    CHECK(p.get_int("default") == 1 && p.at("items").as_list().size() == 2 && !p.contains("values"));
    const phosg::JSON& k = *ctl[3];
    CHECK(k.get_int("default") == 1 && !k.contains("items"));
  }

  // COVERS.md §2.7: every package gains "cover" after "modules"; a generated
  // cover is its origin alone, a picture carries the rest.
  CatalogPackage gen, pic, mine;
  gen.id = "deluxe";
  pic.id = "simpsons";
  pic.cover.origin = pic.cover.original = "download";
  pic.cover.tile = "covers/simpsons/tile.png";
  pic.cover.tile_md5 = std::string(32, 'a');
  pic.cover.image = "covers/simpsons/original.png";
  pic.cover.width = 600;
  pic.cover.height = 776;
  pic.cover.art = "box";
  pic.cover.label = "Box front";
  pic.cover.credit = "Wikisimpsons";
  mine.id = "tt";
  mine.cover = pic.cover;
  mine.cover.origin = "user";
  mine.cover.original = "disc";
  mine.cover.art.clear();
  mine.cover.label = "Your own picture";
  mine.cover.credit.clear();
  // Given in registry order; written oldest release first: simpsons (1994), tt (1995), deluxe (1996).
  std::string text = render_catalog_json({}, {gen, pic, mine});
  CHECK(text.find("   \"modules\": 0,\n   \"cover\": {\n    \"origin\": \"generated\"\n   }\n  }") !=
        std::string::npos);
  CHECK(text.find("   \"cover\": {\n    \"origin\": \"download\",\n    \"tile\": \"covers/simpsons/tile.png\",\n"
                  "    \"tileMd5\": \"" + std::string(32, 'a') + "\",\n    \"image\": \"covers/simpsons/original.png\",\n"
                  "    \"width\": 600,\n    \"height\": 776,\n    \"art\": \"box\",\n    \"label\": \"Box front\",\n"
                  "    \"credit\": \"Wikisimpsons\",\n    \"original\": \"download\"\n   }") != std::string::npos);
  phosg::JSON packages = phosg::JSON::parse(text).at("packages");
  CHECK_EQ(packages.as_list().at(0)->get_string("id"), std::string("simpsons"));
  CHECK_EQ(packages.as_list().at(1)->get_string("id"), std::string("tt"));
  CHECK_EQ(packages.as_list().at(2)->get_string("id"), std::string("deluxe"));
  CHECK_EQ(packages.as_list().at(2)->at("cover").as_dict().size(), size_t(1));
  const phosg::JSON& u = packages.as_list().at(1)->at("cover");
  CHECK_EQ(u.get_string("origin"), std::string("user"));
  CHECK_EQ(u.get_string("original"), std::string("disc"));
  CHECK(!u.contains("art"));
}

// ---- adimport --catalog-only, as a library call ------------------------------------------

void test_regenerate(const fs::path& dir) {
  // Nothing imported: nothing to catalogue, and nothing written.
  CatalogResult r = regenerate_catalog(dir / L"empty");
  CHECK_EQ(r.status, Status::source_invalid);
  CHECK(!fs::exists(dir / L"empty" / L"win" / L"catalog-win.json"));

  fs::path root = dir / L"assets";
  write_tree(root / L"win" / L"FILES");
  std::vector<std::string> logged;
  r = regenerate_catalog(root, [&](const std::string& s) { logged.push_back(s); });
  CHECK_EQ(r.status, Status::ok);
  CHECK_EQ(r.modules, size_t(5));
  CHECK_EQ(r.controls, size_t(3 + 3 + 3 + 3 + 3));
  CHECK_EQ(r.path, root / L"win" / L"catalog-win.json");
  CHECK_EQ(logged.size(), size_t(2));
  std::string text = test::read_text(r.path);
  // PACKAGES.md §6: the installed packages (Deluxe, with no import record) are listed too.
  CatalogTree tree;
  tree.package = find_package("deluxe");
  tree.dir = root / L"win" / L"FILES";
  tree.verified = "unknown";
  CHECK_EQ(text, render_catalog(build_catalog({tree})));
  CHECK_EQ(scan_catalog(root / L"win" / L"FILES").size(), size_t(5));
  phosg::JSON doc = phosg::JSON::parse(text);
  CHECK_EQ(doc.at("modules").as_list().size(), size_t(5));
  for (auto& e : fs::directory_iterator(root / L"win"))
    CHECK(e.path().filename().wstring().find(L".tmp-") == std::wstring::npos);

  // A root that is itself the win directory is updated in place.
  r = regenerate_catalog(root / L"win");
  CHECK_EQ(r.status, Status::ok);
  CHECK_EQ(r.path, root / L"win" / L"catalog-win.json");

  // An import holding the lock: refused, the old catalog left alone.
  {
    Handle lock(CreateFileW((root / L"win" / L"import.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr));
    CHECK(lock.valid());
    test::write_bytes(r.path, {'o', 'l', 'd'});
    r = regenerate_catalog(root);
    CHECK_EQ(r.status, Status::error);
    CHECK(r.message.find("running") != std::string::npos);
    CHECK_EQ(test::read_text(r.path), std::string("old"));
  }
  CHECK_EQ(regenerate_catalog(root).status, Status::ok);
}

// ---- the real corpus ----------------------------------------------------------------------

struct Diff {
  int count = 0;
  void report(const std::string& path, const std::string& what) {
    if (++count <= 40) fprintf(stderr, "  %s: %s\n", path.c_str(), what.c_str());
  }
};

std::string brief(const phosg::JSON& j) {
  std::string s = j.serialize();
  return s.size() > 120 ? s.substr(0, 117) + "..." : s;
}

// Same value, member order aside.
void compare(const phosg::JSON& a, const phosg::JSON& b, const std::string& path, Diff& d) {
  if (a.is_dict() && b.is_dict()) {
    std::set<std::string> keys;
    for (auto& [k, v] : a.as_dict()) keys.insert(k);
    for (auto& [k, v] : b.as_dict()) keys.insert(k);
    for (const auto& k : keys) {
      if (!a.contains(k)) d.report(path + "." + k, "only in the reference: " + brief(b.at(k)));
      else if (!b.contains(k)) d.report(path + "." + k, "only in ours: " + brief(a.at(k)));
      else compare(a.at(k), b.at(k), path + "." + k, d);
    }
  } else if (a.is_list() && b.is_list()) {
    const auto &la = a.as_list(), &lb = b.as_list();
    if (la.size() != lb.size())
      d.report(path, "length " + std::to_string(la.size()) + " vs " + std::to_string(lb.size()));
    for (size_t i = 0; i < std::min(la.size(), lb.size()); i++) {
      std::string sub = path + "[" + std::to_string(i) + "]";
      // Name list entries by id where there is one: easier to read.
      if (la[i]->is_dict() && la[i]->contains("id") && la[i]->at("id").is_string())
        sub = path + "[" + la[i]->at("id").as_string() + "]";
      compare(*la[i], *lb[i], sub, d);
    }
  } else if (a.is_int() && b.is_int()) {
    if (a.as_int() != b.as_int()) d.report(path, brief(a) + " vs " + brief(b));
  } else if (a.is_string() && b.is_string()) {
    if (a.as_string() != b.as_string()) d.report(path, brief(a) + " vs " + brief(b));
  } else if (!(a.is_null() && b.is_null())) {
    d.report(path, "type differs: " + brief(a) + " vs " + brief(b));
  }
}

size_t count_controls(const phosg::JSON& doc) {
  size_t n = 0;
  for (auto& m : doc.at("modules").as_list()) n += m->at("controls").as_list().size();
  return n;
}

// The generator field names the tool, so it is the one expected difference;
// the fields PACKAGES.md §6 added (per module: package, packageTitle,
// moduleName, md5, sameAs; top level: packages) are not the prototype's and
// are ignored here (import.pkg_real checks them).
void check_same_as_reference(const std::string& ours_text, const phosg::JSON& ref, const char* what) {
  phosg::JSON ours = phosg::JSON::parse(ours_text);
  CHECK_EQ(ours.get_string("generator"), std::string(kCatalogGenerator));
  ours.as_dict().erase("generator");
  CHECK(ours.contains("packages"));
  ours.as_dict().erase("packages");
  for (auto& m : ours.at("modules").as_list()) {
    CHECK(m->contains("package") && m->contains("moduleName") && m->contains("md5"));
    for (const char* k : {"package", "packageTitle", "moduleName", "md5", "sameAs"}) m->as_dict().erase(k);
  }
  Diff d;
  compare(ours, ref, "$", d);
  if (d.count) {
    test::g_failures++;
    fprintf(stderr, "%s: %d difference(s) from the prototype's catalog\n", what, d.count);
  }
  size_t ours_modules = ours.at("modules").as_list().size(), ours_controls = count_controls(ours);
  fprintf(stderr, "%s: %zu modules, %zu controls (reference %zu, %zu)\n", what, ours_modules, ours_controls,
          ref.at("modules").as_list().size(), count_controls(ref));
}

// `installed`: the user's assets root (test::installed_assets_root()), read only.
int test_real(const fs::path& scratch, const fs::path& adimport, const fs::path& reference, const fs::path& installed) {
  fs::path win = win_assets_dir(installed);
  std::error_code ec;
  if (installed.empty() || !fs::is_directory(win / L"FILES" / L"AD40", ec) || !fs::is_regular_file(reference, ec)) {
    fprintf(stderr, "SKIP: needs imported assets (%s) and the prototype's %s\n", to_utf8(win.wstring()).c_str(),
            to_utf8(reference.wstring()).c_str());
    return 77;
  }
  phosg::JSON ref = phosg::JSON::parse(test::read_text(reference));
  ref.as_dict().erase("generator");
  // Known prototype errata, corrected in the reference before comparing so
  // that nothing else may differ. make_catalog.py asks pefile for the import
  // directory only (parse_data_directories(directories=[IMPORT])), so its
  // export list is always empty and every module gets entry "Module"; the
  // MSVC-built STARRYNI.AD exports "_Module@4" (ABI.md §1, §2.14, VERIFIED),
  // which is what the C++ generator reports.
  struct Erratum {
    const char *id, *field, *prototype, *actual;
  };
  for (const Erratum& e : {Erratum{"ad40.starryni", "entry", "Module", "_Module@4"}}) {
    for (auto& m : ref.at("modules").as_list()) {
      if (m->get_string("id") != e.id || m->get_string(e.field) != e.prototype) continue;
      m->as_dict()[e.field] = std::make_unique<phosg::JSON>(std::string(e.actual));
      fprintf(stderr, "reference erratum applied: %s.%s \"%s\" -> \"%s\"\n", e.id, e.field, e.prototype, e.actual);
    }
  }
  // The prototype's run over this corpus: 84 modules, 248 controls (ABI.md
  // §2.10.5). A different reference means a different corpus or prototype.
  CHECK_EQ(ref.at("modules").as_list().size(), size_t(84));
  CHECK_EQ(count_controls(ref), size_t(248));

  std::vector<std::string> skipped;
  auto mods = scan_catalog(win / L"FILES", [&](const std::string& s) { skipped.push_back(s); });
  for (auto& s : skipped) fprintf(stderr, "  %s\n", s.c_str());
  CHECK(skipped.empty());
  check_same_as_reference(render_catalog_json(mods), ref, "scan_catalog");

  // The same through adimport --catalog-only, on a scratch copy of just the
  // module files (the user's own catalog is never touched by a test).
  fs::path root = scratch / L"real-assets";
  fs::remove_all(root, ec);
  for (const auto& m : mods) {
    fs::path from = win / to_wide(m.path), to = root / L"win" / to_wide(m.path);
    fs::create_directories(to.parent_path());
    fs::copy_file(from, to, fs::copy_options::overwrite_existing);
  }
  test::ProcessResult pr = test::run_process(adimport.wstring(), {L"--catalog-only", L"--dest", root.wstring()}, 120000);
  fprintf(stderr, "[adimport --catalog-only] exit %d\n%s", pr.exit_code, pr.output.c_str());
  CHECK_EQ(pr.exit_code, 0);
  check_same_as_reference(test::read_text(root / L"win" / L"catalog-win.json"), ref, "adimport --catalog-only");
  fs::remove_all(root, ec);   // copies of the user's files: never left lying around
  return test::finish("import.catalog_real");
}

}  // namespace

int main(int argc, char** argv) {
  std::string suite = argc > 1 ? argv[1] : "unit";
  if (suite == "real") {
    if (argc < 5) {
      fprintf(stderr, "usage: test_import_catalog real <scratch> <adimport.exe> <reference.json>\n");
      return 2;
    }
    // The installed assets, found read only before the sandbox hides them.
    const fs::path installed = test::installed_assets_root();
    fs::path scratch = test::scratch(argc - 1, argv + 1, "adw-import-catalog-real");
    test::sandbox_data_root(scratch / L"localappdata");
    return test_real(scratch, fs::absolute(argv[3]), fs::absolute(argv[4]), installed);
  }
  fs::path dir = test::scratch(argc - 1, argv + 1, "adw-import-catalog");
  test::sandbox_data_root(dir / L"localappdata");  // no default may reach the real data folder
  test_text();
  test_records();
  test_modules(dir / L"modules");
  test_scan(dir / L"scan");
  test_json();
  test_regenerate(dir / L"regen");
  return test::finish("import.catalog");
}
