// main.cpp
//
// Qt GUI のエントリポイント。MainWindow を表示する。
// 画像の一覧取得・ダウンロードは CanonCamera API 経由（MainWindow が利用）。

#include <QApplication>
#include <src/MainWindow.h>

int main(int argc, char **argv)
{
	QApplication a(argc, argv);
	MainWindow w;
	w.show();
	return a.exec();
}
