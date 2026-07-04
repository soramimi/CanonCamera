#include "MainWindow.h"
#include "ui_MainWindow.h"
#include "CanonCamera.h"
#include "MemoryReader.h"
#include "ImageView.h"
#include <cstdio>
#include <memory>

struct MainWindow::Private {
	std::shared_ptr<CanonCamera> cam;
	std::vector<CanonCamera::Item> items;
};

MainWindow::MainWindow(QWidget *parent)
	: QMainWindow(parent)
	, ui(new Ui::MainWindow)
	, m(new Private)
{
	ui->setupUi(this);
}

MainWindow::~MainWindow()
{
	closeCamera();
	delete m;
	delete ui;
}

void MainWindow::closeCamera()
{
	if (m->cam) {
		m->cam->close();
		m->cam.reset();
	}
}

void MainWindow::on_pushButton_clicked()
{
	char const *ip = "192.168.1.2";

	if (!m->cam) {
		m->cam = std::make_shared<CanonCamera>();
		fprintf(stderr, "[*] opening %s ...\n", ip);
		if (!m->cam->open(ip)) {
			fprintf(stderr, "[!] open failed: %s\n", m->cam->lastError().c_str());
			closeCamera();
			return;
		}
		fprintf(stderr, "[+] connected.\n");
	}

	// 第2引数で絞り込み指定: jpeg / raw / all（既定 all）
	CanonCamera::Filter filter = CanonCamera::Filter::All;

	filter = CanonCamera::Filter::Jpeg;
	// filter = CanonCamera::Filter::Raw;

	m->items = m->cam->list(filter);
	fprintf(stderr, "[+] %zu image(s):\n", m->items.size());
	for (size_t i = 0; i < m->items.size() && i < 20; i++) {
		CanonCamera::Item const &it = m->items[i];
		fprintf(stderr, "    0x%08x fmt=0x%04x %10u bytes  %s%s\n",
			it.handle, it.format, it.size, it.filename.c_str(),
			it.isRaw() ? "  [RAW]" : (it.isJpeg() ? "  [JPEG]" : ""));
	}

	fprintf(stderr, "[*] done.\n");

	{
		QStringList columns = {
			"handle",
			"storageId",
			"format",
			"size",
			"filename"
		};

		// 列数・行数を先に確定させてから、ヘッダーとセルを設定する。
		ui->tableWidget->clearContents();
		ui->tableWidget->setColumnCount(columns.size());
		ui->tableWidget->setRowCount(int(m->items.size()));

		for (int i = 0; i < columns.size(); i++) {
			ui->tableWidget->setHorizontalHeaderItem(i, new QTableWidgetItem(columns[i]));
		}

		for (int i = 0; i < int(m->items.size()); i++) {
			CanonCamera::Item const &it = m->items[i];
			ui->tableWidget->setItem(i, 0, new QTableWidgetItem(QString::number(it.handle, 16)));
			ui->tableWidget->setItem(i, 1, new QTableWidgetItem(QString::number(it.storageId)));
			ui->tableWidget->setItem(i, 2, new QTableWidgetItem(QString::number(it.format, 16)));
			ui->tableWidget->setItem(i, 3, new QTableWidgetItem(QString::number(it.size)));
			ui->tableWidget->setItem(i, 4, new QTableWidgetItem(QString::fromStdString(it.filename)));
		}
	}
}


void MainWindow::on_tableWidget_itemDoubleClicked(QTableWidgetItem *item)
{
	int row = item->row();
	if (row >= 0 && row < int(m->items.size())) {
		CanonCamera::Item const &it = m->items[row];
		fprintf(stderr, "[*] downloading %s ...\n", it.filename.c_str());
		if (auto opt = m->cam->get(it)) {
			fprintf(stderr, "[+] downloaded %s (%u bytes)\n", it.filename.c_str(), it.size);
			if (!opt->empty()) {
				MemoryReader reader((char const *)opt->data(), opt->size());
				reader.open(QIODevice::ReadOnly);
				QImage image;
				// フォーマットは内容から自動判定させる（JPEG 以外は表示できないことがある）
				if (image.load(&reader, nullptr)) {
					ui->widget_image->setImage(image);
				} else {
					fprintf(stderr, "[!] cannot decode %s (unsupported format?)\n", it.filename.c_str());
				}
			}
		} else {
			fprintf(stderr, "[!] get failed: %s\n", m->cam->lastError().c_str());
		}
	}
}

