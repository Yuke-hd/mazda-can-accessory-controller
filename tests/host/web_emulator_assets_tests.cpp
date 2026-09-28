#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "replay/web_emulator_assets.hpp"

#include <iterator>
#include <regex>
#include <string>
#include <string_view>

namespace {

std::string asset_body(const std::string_view target) {
  const replay::WebEmulatorAsset *asset = replay::find_web_emulator_asset(target);
  REQUIRE(asset != nullptr);
  return std::string(asset->body);
}

bool contains(const std::string &text, const std::string_view needle) {
  return text.find(needle) != std::string::npos;
}

bool matches(const std::string &text, const char *pattern) {
  return std::regex_search(text, std::regex(pattern, std::regex::ECMAScript | std::regex::icase));
}

// requestAnimationFrame only schedules repaints of already-streamed frames.
std::string without_repaint_scheduler(std::string script) {
  const std::string scheduler{"requestAnimationFrame"};
  for (auto position = script.find(scheduler); position != std::string::npos;
       position = script.find(scheduler, position))
    script.erase(position, scheduler.size());
  return script;
}

} // namespace

TEST_CASE("web emulator serves embedded HTML, CSS, and JavaScript with their media types") {
  const struct {
    std::string_view target;
    std::string_view content_type;
  } expected[]{
      {"/", "text/html; charset=utf-8"},
      {"/index.html", "text/html; charset=utf-8"},
      {"/style.css", "text/css; charset=utf-8"},
      {"/app.js", "text/javascript; charset=utf-8"},
  };
  for (const auto &route : expected) {
    CAPTURE(route.target);
    const replay::WebEmulatorAsset *asset = replay::find_web_emulator_asset(route.target);
    REQUIRE(asset != nullptr);
    CHECK(asset->content_type == route.content_type);
    CHECK_FALSE(asset->body.empty());
  }
  CHECK(replay::find_web_emulator_asset("/missing.js") == nullptr);
  CHECK(replay::find_web_emulator_asset("/../app.js") == nullptr);
}

TEST_CASE("web emulator page provides the strip, replay time, state, and region guides") {
  const std::string html = asset_body("/");
  CHECK(contains(html, "id=\"strip\""));
  CHECK(contains(html, "id=\"replay-time\""));
  CHECK(contains(html, "id=\"status\""));
  CHECK(contains(html, "id=\"guides\""));
  CHECK(contains(html, "src=\"/app.js\""));
  CHECK(contains(html, "href=\"/style.css\""));
  CHECK_FALSE(matches(html, "(https?|wss?)://|(src|href)=\"//"));
}

// Header-driven sizing is covered behaviourally by the Node renderer tests.
// Here we only ensure the production length is not hard-coded elsewhere.
TEST_CASE("web emulator renderer names the production strip length only once") {
  const std::string script = asset_body("/app.js");
  const std::regex literal_100("\\b100\\b");
  CHECK(std::distance(std::sregex_iterator(script.begin(), script.end(), literal_100),
                      std::sregex_iterator()) == 1);
  CHECK(contains(script, "kProductionPixelCount = 100;"));
}

TEST_CASE("web emulator JavaScript contains no vehicle decoding, rules, or animation") {
  const std::string script = without_repaint_scheduler(asset_body("/app.js"));
  CHECK_FALSE(matches(script, "0x[0-9a-f]+"));     // No CAN identifiers or payload masks.
  CHECK_FALSE(matches(script, "\\b(514|145)\\b")); // Engine and turn-switch IDs in decimal.
  CHECK_FALSE(matches(script, "\\brpm\\b|engine|speed|throttle"));
  CHECK_FALSE(matches(script, "\\bturn|hazard|blinker|indicator|red.?zone|brake"));
  CHECK_FALSE(matches(script, "\\b(action|rule|threshold|fill)s?\\b"));
  CHECK_FALSE(
      matches(script, "setInterval|setTimeout|animat|interpolat|\\b(lerp|tween|ease|fade)"));
  CHECK_FALSE(matches(script, "Math\\.(sin|cos|round|min|max)"));
  CHECK_FALSE(matches(script, "(https?|wss?)://")); // No external network endpoints.
}

TEST_CASE("web emulator stylesheet cannot visually mix streamed pixel states") {
  const std::string css = asset_body("/style.css");
  CHECK_FALSE(matches(css, "transition|animation|@keyframes|filter\\s*:|opacity\\s*:"));
  CHECK_FALSE(matches(css, "mix-blend-mode"));
  CHECK_FALSE(matches(css, "(https?:)?//|@import"));
}
