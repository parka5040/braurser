add_executable(braurser-askpass-gtk main.cpp)
target_link_libraries(braurser-askpass-gtk PRIVATE braurser-gtk-kit braurser-presenter)
braurser_harden(braurser-askpass-gtk)
install(TARGETS braurser-askpass-gtk RUNTIME DESTINATION libexec/braurser)
