TEMPLATE = app
CONFIG += console c++17
CONFIG -= app_bundle
CONFIG -= qt

DESTDIR = $$PWD/bin

# 日本語コメントを含む UTF-8 ソースを MSVC が CP932 と誤読するのを防ぐ
msvc: QMAKE_CXXFLAGS += /utf-8

SOURCES += \
        main.cpp \
        canoncamera.cpp

HEADERS += \
        canoncamera.h

# ptpip_prototype.cpp は初期プロトタイプ(単体で main を持つ)。
# 参照用に残すが、ビルドには含めない。
