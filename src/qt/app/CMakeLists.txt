if(NOT TARGET braurser-presenter)
    return()
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(QT_APP_GIO REQUIRED IMPORTED_TARGET gio-unix-2.0)
add_library(braurser-qt-app STATIC platform.cpp widgets.cpp window.cpp catalog.cpp operations.cpp library.cpp)
target_include_directories(braurser-qt-app PUBLIC "${CMAKE_SOURCE_DIR}/include/braurser/qt")
target_link_libraries(braurser-qt-app PUBLIC braurser-qt-kit braurser-presenter PRIVATE PkgConfig::QT_APP_GIO)
braurser_harden(braurser-qt-app)

add_library(braurser-qt-fixtures STATIC fixtures.cpp)
target_link_libraries(braurser-qt-fixtures PUBLIC braurser-qt-app braurser-presenter-fixtures)
braurser_harden(braurser-qt-fixtures)

add_executable(braurser-qt main.cpp)
target_link_libraries(braurser-qt PRIVATE braurser-qt-app braurser-qt-fixtures)
braurser_harden(braurser-qt)
install(TARGETS braurser-qt RUNTIME DESTINATION bin)
