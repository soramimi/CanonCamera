// canoncamera.h
//
// Canon EOS カメラ (Wi-Fi / PTP-IP) へアクセスする最小 API。
// EOS Utility と同じ PTP/IP プロトコルで画像一覧の取得とダウンロードを行う。
//
//   CanonCamera cam;
//   if (cam.open("192.168.1.2")) {
//       for (auto& it : cam.list())
//           cam.get(it, it.filename.c_str());
//       cam.close();
//   }
//
// 注意: Winsock の詳細は .cpp に隠蔽している（このヘッダは winsock2.h に非依存）。

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class CanonCamera {
public:
	// カメラ上の 1 オブジェクト（画像ファイル）を表す。
	struct Item {
		uint32_t handle = 0; // PTP オブジェクトハンドル
		uint32_t storageId = 0; // 所属ストレージ ID
		uint16_t format = 0; // ObjectFormat コード
		uint32_t size = 0; // バイト数（ObjectInfo より）
		std::string filename; // 例 "IMG_1395.CR3"

		// よく使う形式の判定（0x3801=EXIF/JPEG, 0xb1xx=Canon RAW 系）
		bool isJpeg() const { return format == 0x3801; }
		bool isRaw() const { return (format & 0xff00) == 0xb100; }
	};

	// list() の絞り込み対象。
	enum class Filter {
		All, // 画像ファイルすべて
		Jpeg, // JPEG のみ (0x3801)
		Raw, // Canon RAW のみ (0xb1xx)
	};

	CanonCamera();
	~CanonCamera();

	CanonCamera(CanonCamera const &) = delete;
	CanonCamera &operator=(CanonCamera const &) = delete;

	// カメラに接続し、PTP/IP ハンドシェイクとセッション開始まで行う。
	// ip はカメラの IPv4 アドレス。成功で true。
	bool open(std::string const &ip);

	// セッションを閉じて切断する。デストラクタからも呼ばれる。
	void close();

	bool isOpen() const;

	// 画像ファイル（フォルダ=association を除く）の一覧を返す。
	// filter で JPEG のみ / RAW のみに絞り込める（既定は全画像）。
	// JPEG/RAW 指定時はカメラ側フォーマット絞り込みも併用して高速化する。
	// 各要素の GetObjectInfo を取得するため件数に比例した時間がかかる。
	// 失敗時は空 vector を返し lastError() に理由が入る。
	std::vector<Item> list(Filter filter = Filter::All);

	// item をダウンロードして savepath に保存する。成功で true。
	bool get(Item const &item, char const *savepath);

	// クライアント GUID を差し替える（16 バイト, ワイヤ上のバイト順）。
	// open() より前に呼ぶこと。省略時は既定値を使う。
	void setClientGuid(uint8_t const guid[16]);

	// 直近に失敗した処理のエラー内容。
	std::string const &lastError() const { return error_; }

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
	std::string error_;

	bool fail(std::string const &msg); // error_ にセットして false を返す
};
