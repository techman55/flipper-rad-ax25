#!/usr/bin/env ruby
# Regenerates mac/FlipperTNC.xcodeproj (the checked-in project is the output of this).
#
#   gem install xcodeproj && ruby mac/tools/generate_xcodeproj.rb
#
# Layout:
#   FlipperTNC/       SwiftUI app sources + Assets.xcassets
#   FlipperTNCCore/   local Swift package (protocol, serial I/O), linked into the app
require "xcodeproj"

MAC = File.expand_path("..", __dir__)
PROJECT_PATH = File.join(MAC, "FlipperTNC.xcodeproj")
APP = "FlipperTNC"
BUNDLE_ID = "io.github.techman55.FlipperTNC"

FileUtils.rm_rf(PROJECT_PATH)
project = Xcodeproj::Project.new(PROJECT_PATH, false, 60)

target = project.new_target(:application, APP, :osx, "14.0", nil, :swift)

# SwiftUI app: drop the template's explicit Cocoa.framework link.
target.frameworks_build_phase.files.dup.each do |bf|
  next unless bf.file_ref && bf.file_ref.path.to_s.end_with?("Cocoa.framework")
  ref = bf.file_ref
  target.frameworks_build_phase.remove_build_file(bf)
  ref.remove_from_project
end
project.frameworks_group.remove_from_project

# --- sources and resources ---
group = project.main_group.new_group(APP, APP)
%w[FlipperTNCApp.swift AppModel.swift Views.swift].each do |f|
  target.source_build_phase.add_file_reference(group.new_reference(f))
end
target.resources_build_phase.add_file_reference(group.new_reference("Assets.xcassets"))

# --- local Swift package: FlipperTNCCore ---
pkg = project.new(Xcodeproj::Project::Object::XCLocalSwiftPackageReference)
pkg.relative_path = "FlipperTNCCore"
project.root_object.package_references << pkg

product = project.new(Xcodeproj::Project::Object::XCSwiftPackageProductDependency)
product.product_name = "FlipperTNCCore"
target.package_product_dependencies << product

link = project.new(Xcodeproj::Project::Object::PBXBuildFile)
link.product_ref = product
target.frameworks_build_phase.files << link

# --- build settings ---
target.build_configurations.each do |config|
  s = config.build_settings
  s.delete("INFOPLIST_FILE")
  s.merge!(
    "PRODUCT_NAME" => "$(TARGET_NAME)",
    "PRODUCT_BUNDLE_IDENTIFIER" => BUNDLE_ID,
    "MARKETING_VERSION" => "1.0.0",
    "CURRENT_PROJECT_VERSION" => "1",
    "MACOSX_DEPLOYMENT_TARGET" => "14.0",
    "SWIFT_VERSION" => "5.0",
    "GENERATE_INFOPLIST_FILE" => "YES",
    "INFOPLIST_KEY_CFBundleDisplayName" => "Flipper TNC",
    "INFOPLIST_KEY_LSApplicationCategoryType" => "public.app-category.utilities",
    "INFOPLIST_KEY_NSHumanReadableCopyright" => "",
    "ASSETCATALOG_COMPILER_APPICON_NAME" => "AppIcon",
    "ASSETCATALOG_COMPILER_GLOBAL_ACCENT_COLOR_NAME" => "AccentColor",
    "COMBINE_HIDPI_IMAGES" => "YES",
    "LD_RUNPATH_SEARCH_PATHS" => ["$(inherited)", "@executable_path/../Frameworks"],
    "ENABLE_PREVIEWS" => "YES",
    # Local use: ad-hoc "Sign to Run Locally", no Apple developer account needed.
    "CODE_SIGN_STYLE" => "Automatic",
    "CODE_SIGN_IDENTITY" => "-",
    "DEVELOPMENT_TEAM" => "",
    "ENABLE_HARDENED_RUNTIME" => "YES",
    # No App Sandbox: the app lists /dev and opens /dev/cu.* serial devices.
    "ENABLE_APP_SANDBOX" => "NO",
    "ENABLE_USER_SCRIPT_SANDBOXING" => "YES",
  )
end

project.build_configurations.each do |config|
  config.build_settings["MACOSX_DEPLOYMENT_TARGET"] = "14.0"
  config.build_settings["SWIFT_VERSION"] = "5.0"
end

# Project format = Xcode 15 (objectVersion 60); the gem defaults this to 100.
project.root_object.preferred_project_object_version = "60" if project.root_object.respond_to?(:preferred_project_object_version=)
project.root_object.attributes["LastSwiftUpdateCheck"] = "1600"
project.root_object.attributes["LastUpgradeCheck"] = "1600"
project.root_object.attributes["TargetAttributes"] = {
  target.uuid => { "CreatedOnToolsVersion" => "16.0" },
}
project.predictabilize_uuids # stable IDs: regenerating gives a minimal diff
project.save

# --- shared scheme (so `xcodebuild -scheme FlipperTNC` works before Xcode ever opens it) ---
scheme = Xcodeproj::XCScheme.new
scheme.add_build_target(target)
scheme.set_launch_target(target)
scheme.launch_action.build_configuration = "Debug"
scheme.archive_action.build_configuration = "Release"
scheme.profile_action.build_configuration = "Release"
scheme.save_as(PROJECT_PATH, APP, true)

puts "Wrote #{PROJECT_PATH}"
