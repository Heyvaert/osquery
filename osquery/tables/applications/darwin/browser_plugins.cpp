/*
 *  Copyright (c) 2014, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include <osquery/filesystem.h>
#include <osquery/tables.h>

/// Include the "external" (not OS X provided) libarchive header.
#include <archive.h>
#include <archive_entry.h>

namespace fs = boost::filesystem;
namespace pt = boost::property_tree;

namespace osquery {
namespace tables {

/// Each home directory will include custom extensions.
#define kSafariExtensionsPath "/Library/Safari/Extensions/"

/// Safari extensions will not load unless they have the expected pattern.
#define kSafariExtensionsPattern "%.safariextz"

#define kBrowserPluginsPath "/Library/Internet Plug-Ins/"

const std::map<std::string, std::string> kBrowserPluginKeys = {
    {"WebPluginName", "name"},
    {"CFBundleIdentifier", "identifier"},
    {"CFBundleShortVersionString", "version"},
    {"DTPlatformBuild", "sdk"},
    {"WebPluginDescription", "description"},
    {"CFBundleDevelopmentRegion", "development_region"},
    {"LSRequiresNativeExecution", "native"},
};

const std::map<std::string, std::string> kSafariExtensionKeys = {
    {"CFBundleDisplayName", "name"},
    {"CFBundleIdentifier", "identifier"},
    {"CFBundleShortVersionString", "version"},
    {"Author", "author"},
    {"CFBundleInfoDictionaryVersion", "sdk"},
    {"Description", "description"},
    {"Update Manifest URL", "update_url"},
};

void genBrowserPlugin(const std::string& path, QueryData& results) {
  Row r;
  pt::ptree tree;
  if (osquery::parsePlist(path + "/Contents/Info.plist", tree).ok()) {
    // Plugin did not include an Info.plist, or it was invalid
    for (const auto& it : kBrowserPluginKeys) {
      r[it.second] = tree.get(it.first, "");

      // Convert Plist bool-types to an integer.
      if (r.at(it.second) == "true" || r.at(it.second) == "YES" ||
          r.at(it.second) == "Yes") {
        r[it.second] = "1";
      } else if (r.at(it.second) == "false" || r.at(it.second) == "NO" ||
                 r.at(it.second) == "No") {
        r[it.second] = "0";
      }
    }
  }

  if (r.count("native") == 0 || r.at("native").size() == 0) {
    // The default case for native execution is false.
    r["native"] = "0";
  }

  r["path"] = path;
  results.push_back(std::move(r));
}

QueryData genBrowserPlugins(QueryContext& context) {
  QueryData results;

  std::vector<std::string> bundles;
  if (listDirectoriesInDirectory(kBrowserPluginsPath, bundles).ok()) {
    for (const auto& dir : bundles) {
      genBrowserPlugin(dir, results);
    }
  }

  auto homes = osquery::getHomeDirectories();
  for (const auto& home : homes) {
    bundles.clear();
    if (listDirectoriesInDirectory(home / kBrowserPluginsPath, bundles).ok()) {
      for (const auto& dir : bundles) {
        genBrowserPlugin(dir, results);
      }
    }
  }

  return results;
}

inline void genSafariExtension(const std::string& path, QueryData& results) {
  Row r;
  r["path"] = path;

  // Loop through (Plist key -> table column name) in kSafariExtensionKeys.
  struct archive* ext = archive_read_new();
  if (ext == nullptr) {
    return;
  }

  // Use open_file, instead of the preferred open_filename for OS X 10.9.
  archive_read_support_format_xar(ext);
  if (archive_read_open_file(ext, path.c_str(), 10240) != ARCHIVE_OK) {
    archive_read_close(ext);
    return;
  }

  struct archive_entry* entry = nullptr;
  while (archive_read_next_header(ext, &entry) == ARCHIVE_OK) {
    auto item_path = archive_entry_pathname(entry);
    // Documentation for libarchive mentions these APIs may return NULL.
    if (item_path == nullptr) {
      archive_read_data_skip(ext);
      continue;
    }

    // Assume there is no non-root Info.
    if (std::string(item_path).find("Info.plist") == std::string::npos) {
      archive_read_data_skip(ext);
      continue;
    }

    // Read the decompressed Info.plist content.
    auto content = std::string(archive_entry_size(entry), '\0');
    archive_read_data_into_buffer(ext, &content[0], content.size());

    // If the Plist can be parsed, extract important keys into columns.
    pt::ptree tree;
    if (parsePlistContent(content, tree).ok()) {
      for (const auto& it : kSafariExtensionKeys) {
        r[it.second] = tree.get(it.first, "");
      }
    }
    break;
  }

  archive_read_close(ext);
  results.push_back(std::move(r));
}

QueryData genSafariExtensions(QueryContext& context) {
  QueryData results;

  auto homes = osquery::getHomeDirectories();
  for (const auto& home : homes) {
    auto dir = home / kSafariExtensionsPath;
    // Check that an extensions directory exists.
    if (!pathExists(dir).ok()) {
      continue;
    }

    // Glob the extension files.
    std::vector<std::string> paths;
    if (!resolveFilePattern(dir / kSafariExtensionsPattern, paths).ok()) {
      continue;
    }

    for (const auto& extension_path : paths) {
      genSafariExtension(extension_path, results);
    }
  }

  return results;
}
}
}
