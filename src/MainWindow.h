#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>

class QTableWidgetItem;

namespace Ui {
class MainWindow;
}

class MainWindow : public QMainWindow {
	Q_OBJECT
private:
	struct Private;
	Private *m;
public:
	explicit MainWindow(QWidget *parent = nullptr);
	~MainWindow();

private slots:
	void on_pushButton_clicked();

	void on_tableWidget_itemDoubleClicked(QTableWidgetItem *item);

private:
	Ui::MainWindow *ui;
	void closeCamera();
};

#endif // MAINWINDOW_H
