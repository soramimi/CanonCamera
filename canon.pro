QMAKE_PROJECT_DEPTH=0
TEMPLATE = app
CONFIG += console c++17
CONFIG -= app_bundle
# CONFIG -= qt
QT += core gui widgets

DESTDIR = $$PWD/bin

# 日本語コメントを含む UTF-8 ソースを MSVC が CP932 と誤読するのを防ぐ
msvc: QMAKE_CXXFLAGS += /utf-8

SOURCES += \
        main.cpp \
        canoncamera.cpp \
        src/ImageView.cpp \
        src/MainWindow.cpp \
        src/MemoryReader.cpp

HEADERS += \
        canoncamera.h \
        src/ImageView.h \
        src/MainWindow.h \
        src/MemoryReader.h

# ptpip_prototype.cpp は初期プロトタイプ(単体で main を持つ)。
# 参照用に残すが、ビルドには含めない。

FORMS += \
	src/MainWindow.ui
