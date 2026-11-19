// Application-layer tests need an event loop (background field maps), hence a QCoreApplication.
#include "qsig_test.h"

#include <QCoreApplication>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    return qsig::test::runAll(argc, argv);
}
