#ifndef IMAGEVIEW_H
#define IMAGEVIEW_H

#include <QWidget>

class ImageView : public QWidget {
	Q_OBJECT
private:
	QImage image_;
public:
	explicit ImageView(QWidget *parent = nullptr);

	void setImage(const QImage &newImage);

signals:


	// QWidget interface
protected:
	void paintEvent(QPaintEvent *event);
};

#endif // IMAGEVIEW_H
