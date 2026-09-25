#ifndef BRAURSER_ASKPASS_QT_DIALOG_HPP
#define BRAURSER_ASKPASS_QT_DIALOG_HPP

#include "core.hpp"
#include <QDialog>

namespace braurser::askpass {

class Dialog final : public QDialog {
public:
    Dialog(presenter::Auth& auth, const presenter::Copy& copy);
private:
    presenter::Subscription subscription_;
    bool terminating_ = false;
};

}

#endif
