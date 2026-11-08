#include <QCoreApplication>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QtWebEngineQuick/QtWebEngineQuick>
#include <QtQuickTest/quicktest.h>

int main(int argc, char** argv)
{
  QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
  QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGLRhi);
  QtWebEngineQuick::initialize();
  return quick_test_main(argc, argv, "InputPlugin", nullptr);
}
