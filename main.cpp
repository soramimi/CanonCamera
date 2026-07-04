// main.cpp
//
// CanonCamera API のデモ。カメラに接続して画像一覧を表示し、先頭の1枚を保存する。
//   使い方: canon.exe [camera-ip]

#include "canoncamera.h"
#include <cstdio>

int main(int argc, char **argv)
{
	char const *ip = (argc >= 2) ? argv[1] : "192.168.1.2";

	CanonCamera cam;
	printf("[*] opening %s ...\n", ip);
	if (!cam.open(ip)) {
		printf("[!] open failed: %s\n", cam.lastError().c_str());
		return 1;
	}
	printf("[+] connected.\n");

	// 第2引数で絞り込み指定: jpeg / raw / all（既定 all）
	CanonCamera::Filter filter = CanonCamera::Filter::All;
	if (argc >= 3) {
		std::string f = argv[2];
		if (f == "jpeg")
			filter = CanonCamera::Filter::Jpeg;
		else if (f == "raw")
			filter = CanonCamera::Filter::Raw;
	}

	std::vector<CanonCamera::Item> items = cam.list(filter);
	printf("[+] %zu image(s):\n", items.size());
	for (size_t i = 0; i < items.size() && i < 20; i++) {
		CanonCamera::Item const &it = items[i];
		printf("    0x%08x fmt=0x%04x %10u bytes  %s%s\n",
			it.handle, it.format, it.size, it.filename.c_str(),
			it.isRaw() ? "  [RAW]" : (it.isJpeg() ? "  [JPEG]" : ""));
	}

	// デモ: 先頭の 1 枚を本来のファイル名で保存
	if (!items.empty()) {
		CanonCamera::Item const &it = items[0];
		printf("[*] downloading %s ...\n", it.filename.c_str());
		if (cam.get(it, it.filename.c_str()))
			printf("[+] saved %s (%u bytes)\n", it.filename.c_str(), it.size);
		else
			printf("[!] get failed: %s\n", cam.lastError().c_str());
	}

	cam.close();
	printf("[*] done.\n");
	return 0;
}
