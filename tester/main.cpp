#include <QCoreApplication>
#include <QDir>
#include <QDebug>

#include "pycpptester.h"

int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);

    QString samplespath = QDir::currentPath() + "/../samples/";
    qDebug() << "Using samples path: " << samplespath;
    QString pythonFilePath = samplespath + "PyTest.py";

    PyCppTester tester(pythonFilePath);
    tester.go();

    int ret = a.exec();
    qDebug() << "QCoreApp returned " << ret;
}
