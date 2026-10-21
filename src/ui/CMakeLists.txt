set_source_files_properties("${PROJECT_SOURCE_DIR}/resources/afd.svg" PROPERTIES QT_RESOURCE_ALIAS "resources/afd.svg")
qt_add_executable(alternativefastdelivery main.cpp controller.cpp controller.hpp)
qt_add_qml_module(alternativefastdelivery URI AFD VERSION 1.0
  QML_FILES Main.qml AfdButton.qml
  OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/AFD"
  RESOURCES "${PROJECT_SOURCE_DIR}/resources/afd.svg")
if(ANDROID)
  set(afd_android_package "${CMAKE_CURRENT_BINARY_DIR}/android-package")
  file(COPY "${PROJECT_SOURCE_DIR}/packaging/android/" DESTINATION "${afd_android_package}")
  file(COPY "${PROJECT_SOURCE_DIR}/resources/licenses/" DESTINATION "${afd_android_package}/assets/licenses")
  file(GLOB afd_dependency_notices "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/*/copyright")
  foreach(afd_notice IN LISTS afd_dependency_notices)
    get_filename_component(afd_notice_parent "${afd_notice}" DIRECTORY)
    get_filename_component(afd_notice_port "${afd_notice_parent}" NAME)
    configure_file("${afd_notice}" "${afd_android_package}/assets/licenses/${afd_notice_port}.txt" COPYONLY)
  endforeach()
else()
  set(afd_android_package "${PROJECT_SOURCE_DIR}/packaging/android")
endif()
target_compile_definitions(alternativefastdelivery PRIVATE AFD_VERSION="${AFD_VERSION}" AFD_APP_MAJOR=${AFD_APP_MAJOR} AFD_APP_MINOR=${AFD_APP_MINOR} AFD_APP_PATCH=${AFD_APP_PATCH})
target_link_libraries(alternativefastdelivery PRIVATE afd_core Qt6::Quick Qt6::QuickControls2 Qt6::Network Qt6::Concurrent Qt6::Svg)
if(ANDROID AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  target_link_options(alternativefastdelivery PRIVATE "-Wl,-z,max-page-size=16384")
endif()
set_target_properties(alternativefastdelivery PROPERTIES
  WIN32_EXECUTABLE TRUE MACOSX_BUNDLE TRUE
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  MACOSX_BUNDLE_GUI_IDENTIFIER org.alternativefastdelivery.app
  MACOSX_BUNDLE_BUNDLE_NAME AlternativeFastDelivery
  MACOSX_BUNDLE_SHORT_VERSION_STRING ${AFD_NUMERIC_VERSION}
  MACOSX_BUNDLE_BUNDLE_VERSION ${AFD_NUMERIC_VERSION}
  QT_ANDROID_PACKAGE_NAME org.alternativefastdelivery.app
  QT_ANDROID_APP_NAME AlternativeFastDelivery
  QT_ANDROID_APP_ICON "@drawable/afd"
  QT_ANDROID_PACKAGE_SOURCE_DIR "${afd_android_package}"
  QT_ANDROID_VERSION_NAME "${AFD_VERSION}"
  QT_ANDROID_VERSION_CODE "${AFD_ANDROID_VERSION_CODE}"
  QT_ANDROID_PERMISSIONS "android.permission.INTERNET;android.permission.ACCESS_NETWORK_STATE"
  QT_ANDROID_MIN_SDK_VERSION 28
  QT_ANDROID_TARGET_SDK_VERSION 36)
if(IOS)
  set_target_properties(alternativefastdelivery PROPERTIES
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME AppIcon)
  target_sources(alternativefastdelivery PRIVATE ${PROJECT_SOURCE_DIR}/packaging/ios/Assets.xcassets)
  set_source_files_properties(${PROJECT_SOURCE_DIR}/packaging/ios/Assets.xcassets PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
elseif(APPLE)
  target_sources(alternativefastdelivery PRIVATE ${PROJECT_SOURCE_DIR}/packaging/macos/afd.icns)
  set_source_files_properties(${PROJECT_SOURCE_DIR}/packaging/macos/afd.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  set_target_properties(alternativefastdelivery PROPERTIES MACOSX_BUNDLE_ICON_FILE afd.icns)
elseif(WIN32)
  enable_language(RC)
  target_sources(alternativefastdelivery PRIVATE ${PROJECT_SOURCE_DIR}/packaging/windows/afd.rc)
  target_include_directories(alternativefastdelivery PRIVATE "${PROJECT_SOURCE_DIR}/packaging/windows")
endif()
if(UNIX AND NOT APPLE AND NOT ANDROID)
  set_target_properties(alternativefastdelivery PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")
endif()
if(IOS)
  set_target_properties(alternativefastdelivery PROPERTIES XCODE_ATTRIBUTE_SKIP_INSTALL NO)
endif()
include(GNUInstallDirs)
install(TARGETS alternativefastdelivery BUNDLE DESTINATION .
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
if(WIN32)
  install(FILES $<TARGET_RUNTIME_DLLS:alternativefastdelivery> DESTINATION ${CMAKE_INSTALL_BINDIR})
  if(VCPKG_INSTALLED_DIR AND VCPKG_TARGET_TRIPLET)
    install(DIRECTORY "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/"
      DESTINATION ${CMAKE_INSTALL_BINDIR} FILES_MATCHING PATTERN "*.dll")
  endif()
  set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ${CMAKE_INSTALL_BINDIR})
  include(InstallRequiredSystemLibraries)
endif()
if(NOT ANDROID AND NOT IOS)
  if(UNIX AND NOT APPLE)
    set(QT_DEPLOY_USE_PATCHELF ON)
    find_program(QT_DEPLOY_PATCHELF_EXECUTABLE patchelf REQUIRED)
    install(CODE [[
      set(QT_DEPLOY_LIB_DIR "lib")
      set(QT_DEPLOY_QML_DIR "qml")
      set(QT_DEPLOY_PLUGINS_DIR "plugins")
      set(QT_DEPLOY_TRANSLATIONS_DIR "translations")
    ]])
  endif()
  qt_generate_deploy_qml_app_script(TARGET alternativefastdelivery
    OUTPUT_SCRIPT deploy_script NO_UNSUPPORTED_PLATFORM_ERROR)
  if(UNIX AND NOT APPLE)
    qt_generate_deploy_script(TARGET alternativefastdelivery OUTPUT_SCRIPT deploy_script CONTENT [[
      qt_deploy_qml_imports(TARGET alternativefastdelivery PLUGINS_FOUND plugins_found)
      qt_deploy_runtime_dependencies(
        EXECUTABLE "$<TARGET_FILE:alternativefastdelivery>"
        ADDITIONAL_MODULES ${plugins_found}
        GENERATE_QT_CONF
        INCLUDE_PLUGINS qoffscreen qwayland-generic qwayland-egl qsvg qsvgicon
        POST_EXCLUDE_REGEXES
          ".*/ld-linux[^/]*[.]so.*"
          ".*/lib(c|m|pthread|dl|rt|resolv|util|anl|nss_[^/]*)[.]so.*"
          ".*/lib(GL[^/]*|EGL|OpenGL|drm[^/]*|gbm|glapi|vulkan|nvidia[^/]*)[.]so.*")
    ]])
  endif()
  install(SCRIPT ${deploy_script})
endif()
