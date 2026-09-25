#ifndef BRAURSER_GTK_TEST_DISPLAY_HPP
#define BRAURSER_GTK_TEST_DISPLAY_HPP

#include <gtkmm.h>

namespace braurser::gtk::test {

inline bool display_available() {
    static const bool available = gtk_init_check();
    return available;
}

}

#endif
