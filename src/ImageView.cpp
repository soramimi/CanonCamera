#include "ImageView.h"

#include <QPainter>


ImageView::ImageView(QWidget *parent)
	: QWidget{parent}
{

}

void ImageView::setImage(const QImage &newImage)
{
	image_ = newImage;
	update();
}


void ImageView::paintEvent(QPaintEvent *)
{
	QPainter pr(this);
	if (image_.isNull()) return;
	// ビュー矩形にアスペクト比を保って収め、中央に描画する。
	QImage image = image_.scaled(width(), height(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
	pr.drawImage((width() - image.width()) / 2, (height() - image.height()) / 2, image);
}
