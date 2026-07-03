/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/json/title_gamerpics_object_json.h"
#include "xenia/base/string_util.h"

namespace xe {
namespace kernel {
TitleGamerpicsObjectJSON::TitleGamerpicsObjectJSON() {}

TitleGamerpicsObjectJSON::~TitleGamerpicsObjectJSON() {}

// Helpers for tolerating both the legacy gamerpic JSON and the minimal shape
// xboxgamer.pics switched to in 2026 (which dropped name/url_small/cdn/cdn_small
// and made `image` a full URL). See CONTRACT.md in the gamerpics-mirror repo.

// Last path segment of a URL or path, with any query/fragment stripped.
static std::string GamerpicBaseName(const std::string& url) {
  std::string s = url;
  const auto q = s.find_first_of("?#");
  if (q != std::string::npos) {
    s = s.substr(0, q);
  }
  const auto slash = s.find_last_of('/');
  return slash == std::string::npos ? s : s.substr(slash + 1);
}

// Tile id from a bare filename "<hex>.png" (0 if not parseable).
static uint32_t GamerpicTileId(const std::string& filename) {
  if (filename.empty()) {
    return 0;
  }
  const auto dot = filename.find_last_of('.');
  const std::string stem =
      dot == std::string::npos ? filename : filename.substr(0, dot);
  if (stem.empty()) {
    return 0;
  }
  return string_util::from_string<uint32_t>(stem, true);
}

// Path portion of an absolute URL ("https://host/a/b.png" -> "/a/b.png"); a
// value already given as a path is returned with a leading slash ensured.
static std::string GamerpicUrlPath(const std::string& url) {
  if (url.empty()) {
    return {};
  }
  const auto scheme = url.find("://");
  if (scheme == std::string::npos) {
    return url.front() == '/' ? url : "/" + url;
  }
  const auto path_start = url.find('/', scheme + 3);
  return path_start == std::string::npos ? "/" : url.substr(path_start);
}

bool TitleGamerpicsObjectJSON::Deserialize(const rapidjson::Value& obj) {
  GameTitle title = {};

  if (obj.HasMember("_id") && !obj["_id"].IsNull()) {
    title.id = string_util::from_string<uint32_t>(obj["_id"].GetString(), true);
  }

  if (obj.HasMember("gamerpics") && obj["gamerpics"].IsArray() &&
      !obj["gamerpics"].IsNull()) {
    const auto gamerpics = obj["gamerpics"].GetArray();

    for (const auto& gamerpic : gamerpics) {
      title.gamerpics.push_back(DeserializeGamerpic(gamerpic));
    }
  }

  if (obj.HasMember("image") && !obj["image"].IsNull()) {
    // Reduce to a bare filename. GetMultiGameInfo composes
    // "<assets_base>/titles/<id>/<image>", so a full URL (the 2026 upstream
    // shape) would double up; a bare name keeps asset routing on
    // gamerpic_assets_address.
    title.image = GamerpicBaseName(obj["image"].GetString());
  }

  if (obj.HasMember("name") && !obj["name"].IsNull()) {
    title.name = obj["name"].GetString();
  }

  if (obj.HasMember("slug") && !obj["slug"].IsNull()) {
    title.slug = obj["slug"].GetString();
  }

  if (obj.HasMember("type") && !obj["type"].IsNull()) {
    title.type = obj["type"].GetString();
  }

  if (obj.HasMember("url") && !obj["url"].IsNull()) {
    title.url = obj["url"].GetString();
  }

  title_ = title;

  return true;
}

Gamerpic TitleGamerpicsObjectJSON::DeserializeGamerpic(
    const rapidjson::Value& gamerpicObj) {
  Gamerpic gamepic = {};

  const auto get_str = [&](const char* key) -> std::string {
    if (gamerpicObj.HasMember(key) && gamerpicObj[key].IsString()) {
      return gamerpicObj[key].GetString();
    }
    return {};
  };

  gamepic.cdn = get_str("cdn");
  gamepic.id = get_str("id");
  gamepic.name = get_str("name");
  gamepic.url = get_str("url");
  gamepic.url_small = get_str("url_small");
  gamepic.cdn_small = get_str("cdn_small");

  if (gamerpicObj.HasMember("likes") && gamerpicObj["likes"].IsUint()) {
    gamepic.likes = gamerpicObj["likes"].GetUint();
  }

  // Big tile id: legacy `name` stem, else the `url` filename stem, else the
  // compound `id` suffix "<title>-<tile>". The 2026 upstream dropped `name`.
  gamepic.big_tile_id = GamerpicTileId(gamepic.name);
  if (!gamepic.big_tile_id) {
    gamepic.big_tile_id = GamerpicTileId(GamerpicBaseName(gamepic.url));
  }
  if (!gamepic.big_tile_id) {
    const auto dash = gamepic.id.find_last_of('-');
    if (dash != std::string::npos) {
      gamepic.big_tile_id =
          string_util::from_string<uint32_t>(gamepic.id.substr(dash + 1), true);
    }
  }

  // Small tile id: legacy `url_small` stem, else the same image as big (the
  // current model serves one image per gamerpic, so big and small coincide).
  gamepic.small_tile_id = GamerpicTileId(GamerpicBaseName(gamepic.url_small));
  if (!gamepic.small_tile_id) {
    gamepic.small_tile_id = gamepic.big_tile_id;
  }

  // Download path: the browse grid fetches "<assets_base><cdn>" and recovers
  // the tile from the cdn stem, so synthesize a cdn from the asset URL when the
  // upstream no longer provides one.
  if (gamepic.cdn.empty()) {
    gamepic.cdn = GamerpicUrlPath(gamepic.url);
  }
  if (gamepic.cdn_small.empty()) {
    gamepic.cdn_small = gamepic.url_small.empty()
                            ? gamepic.cdn
                            : GamerpicUrlPath(gamepic.url_small);
  }

  assert_not_zero(gamepic.small_tile_id);
  assert_not_zero(gamepic.big_tile_id);

  return gamepic;
}

bool TitleGamerpicsObjectJSON::Serialize(
    rapidjson::PrettyWriter<rapidjson::StringBuffer>* writer) const {
  return false;
}
}  // namespace kernel
}  // namespace xe
