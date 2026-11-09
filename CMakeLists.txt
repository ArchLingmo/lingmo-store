cmake_minimum_required(VERSION 3.16)
project(lingmo-store VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

include(GNUInstallDirs)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Qml Quick DBus Network Sql LinguistTools)
find_package(KF6WindowSystem REQUIRED)
find_package(PkgConfig REQUIRED)

# The catalog: libappstream (package "appstream") reads the Arch and Flathub metadata
pkg_check_modules(APPSTREAM REQUIRED IMPORTED_TARGET appstream>=1.0)
# Optional pieces degrade gracefully when missing at build time
pkg_check_modules(FLATPAK IMPORTED_TARGET flatpak)
pkg_check_modules(ALPM IMPORTED_TARGET libalpm)
pkg_check_modules(JXL IMPORTED_TARGET libjxl)

add_executable(lingmo-store
    src/main.cpp
    src/storeapp.cpp
    src/catalog.cpp
    src/flatpakbackend.cpp
    src/installer.cpp
    src/gamesmanager.cpp
    src/gamecompat.cpp
    src/steamconfig.cpp
    src/reviews.cpp
    src/permissions.cpp
    src/lutrissource.cpp
    src/imageproviders.cpp
    resources.qrc
)

target_link_libraries(lingmo-store PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Qml Qt6::Quick Qt6::DBus Qt6::Network Qt6::Sql
    KF6::WindowSystem
    PkgConfig::APPSTREAM
)
if(FLATPAK_FOUND)
    target_link_libraries(lingmo-store PRIVATE PkgConfig::FLATPAK)
    target_compile_definitions(lingmo-store PRIVATE HAVE_FLATPAK)
else()
    message(WARNING "libflatpak not found: Flathub apps can't be installed")
endif()
if(ALPM_FOUND)
    target_link_libraries(lingmo-store PRIVATE PkgConfig::ALPM)
    target_compile_definitions(lingmo-store PRIVATE HAVE_ALPM)
else()
    message(WARNING "libalpm not found: Arch packages won't be offered")
endif()
if(JXL_FOUND)
    target_link_libraries(lingmo-store PRIVATE PkgConfig::JXL)
    target_compile_definitions(lingmo-store PRIVATE HAVE_JXL)
else()
    message(STATUS "libjxl not found: Arch icons need kimageformats")
endif()

set(HELPER_DIR ${CMAKE_INSTALL_PREFIX}/lib/lingmo-store)
set(TRANSLATIONS_DIR ${CMAKE_INSTALL_FULL_DATADIR}/lingmo-store/translations)
target_compile_definitions(lingmo-store PRIVATE
    TRANSLATIONS_DIR="${TRANSLATIONS_DIR}"
    HELPER_PATH="${HELPER_DIR}/lingmo-store-helper"
    LINGMO_STORE_VERSION="${PROJECT_VERSION}"
    # GLib headers use "signals" as an identifier
    QT_NO_KEYWORDS
)

# lrelease only: building never rewrites the .ts sources
qt_add_translation(QM_FILES translations/lingmo-store_pt_BR.ts)
add_custom_target(translations ALL DEPENDS ${QM_FILES})
install(FILES ${QM_FILES} DESTINATION ${TRANSLATIONS_DIR})

# Unit tests: cmake -DBUILD_TESTING=ON, then ctest (no network, no flatpak)
option(BUILD_TESTING "Build the unit tests" OFF)
if(BUILD_TESTING)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_executable(tst_lutris tests/tst_lutris.cpp src/lutrissource.cpp src/gamecompat.cpp)
    target_include_directories(tst_lutris PRIVATE src)
    target_link_libraries(tst_lutris PRIVATE Qt6::Core Qt6::Network Qt6::Sql Qt6::Test)
    target_compile_definitions(tst_lutris PRIVATE
        LINGMO_STORE_VERSION="${PROJECT_VERSION}"
        FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
        QT_NO_KEYWORDS)
    add_test(NAME tst_lutris COMMAND tst_lutris)
    set_tests_properties(tst_lutris PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

install(TARGETS lingmo-store RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS helper/lingmo-store-helper DESTINATION ${HELPER_DIR})
install(FILES data/com.lingmo.store.policy DESTINATION ${CMAKE_INSTALL_DATADIR}/polkit-1/actions)
install(FILES data/lingmo-store.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
