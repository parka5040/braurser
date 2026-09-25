add_library(braurser-askpass-qt-core STATIC core.cpp)
target_include_directories(braurser-askpass-qt-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(braurser-askpass-qt-core PUBLIC braurser-presenter)
braurser_harden(braurser-askpass-qt-core)

add_library(braurser-askpass-qt-ui STATIC dialog.cpp)
target_link_libraries(braurser-askpass-qt-ui PUBLIC braurser-askpass-qt-core braurser-qt-app)
braurser_harden(braurser-askpass-qt-ui)

add_executable(braurser-askpass-qt main.cpp)
target_link_libraries(braurser-askpass-qt PRIVATE braurser-askpass-qt-ui)
braurser_harden(braurser-askpass-qt)
install(TARGETS braurser-askpass-qt RUNTIME DESTINATION libexec/braurser)
