# AGENTS.md

Canon EOS カメラ (Wi-Fi / PTP-IP) から写真を転送する C++ ツール。
EOS Utility と同じ **PTP/IP** プロトコルを libgphoto2 等に依存せず自前実装している。

## プロジェクト概要

- 目的: Wi-Fi 接続した Canon EOS から画像一覧の取得とダウンロードを行う API とデモを提供する。
- 対象プロトコル: PTP/IP (Picture Transfer Protocol over IP, ISO 15740)。TCP ポート **15740**。
- 実機確認済み: **Canon EOS R10**（「EOS Utility」接続モード）。
- プラットフォーム: Windows (Winsock2)。

## ファイル構成

| ファイル | 役割 |
|---|---|
| `canoncamera.h` | 公開 API。`CanonCamera` クラスと `Item` 構造体。winsock2.h に非依存。 |
| `canoncamera.cpp` | PTP/IP 実装本体。ソケット・フレーミング・オペレーションを pimpl (`Impl`) に隠蔽。 |
| `main.cpp` | API を使うデモ。接続 → 一覧 → 先頭1枚を保存。 |
| `canon.pro` | qmake プロジェクト。`main.cpp` + `canoncamera.cpp` をビルドし `bin/canon.exe` を生成。 |
| `ptpip_prototype.cpp` | 最初の単体プロトタイプ（`main` を持つ）。**参照用**。ビルド対象外。 |
| `run.bat` | `bin\canon.exe 192.168.1.2` を実行するだけ。 |

## ビルド

**重要: ソースは UTF-8（日本語コメント入り）。MSVC は必ず `/utf-8` を付けること。**
付けないと CP932 誤読でコメントのバイト列がコードを壊し、大量の構文エラーになる。
`canon.pro` には `msvc: QMAKE_CXXFLAGS += /utf-8` を設定済み。

Qt Creator / qmake でのビルドが本来の経路。手元での素早い確認は下記ワンライナー:

```
cl /nologo /utf-8 /EHsc /std:c++17 main.cpp canoncamera.cpp ws2_32.lib /Fe:bin\canon.exe /Fo:bin\
```

## 実行

```
bin\canon.exe <camera-ip> [jpeg|raw|all]
```

- 第2引数省略時は全画像。
- カメラを「EOS Utility」接続モードにして同じ LAN に載せておくこと。

## API

```cpp
CanonCamera cam;
if (cam.open("192.168.1.2")) {
    for (const auto& it : cam.list(CanonCamera::Filter::Jpeg))  // Jpeg / Raw / All
        cam.get(it, it.filename.c_str());
    cam.close();
}
// 失敗は戻り値(false/空vector) + cam.lastError() で取得。例外は投げない。
```

- `Item`: `handle / storageId / format / size / filename` と `isJpeg() / isRaw()`。
- `list(Filter)`: 画像ファイル（フォルダ=association を除く）を列挙。
- `get(Item, savepath)`: ダウンロードし、保存後に ObjectInfo のサイズと照合。
- `setClientGuid(guid[16])`: `open()` 前に GUID を差し替え可能。

## プロトコルの要点（PTP/IP）

- フレーミング: `[length(4, LE)][type(4, LE)][payload]`。全フィールド **リトルエンディアン**。
- 接続は **2 本の TCP** を張る:
  1. command チャネル: `Init Command Request`(GUID + FriendlyName(UTF-16LE) + version 0x00010000) → `Init Command Ack`(connection number 取得)。
  2. event チャネル: 別ソケットで `Init Event Request`(connection number) → `Init Event Ack`。
- オペレーション: `Operation Request` → (任意の data-in/out: Start/Data/End Data Packet) → `Operation Response`(先頭2byteが RC, 0x2001=OK)。
- 使用オペコード: OpenSession(0x1002) / CloseSession(0x1003) / GetStorageIDs(0x1004) / GetObjectHandles(0x1007) / GetObjectInfo(0x1008) / GetObject(0x1009)。
- `GetObjectHandles(storageID, formatCode, 0)` の formatCode でカメラ側フォーマット絞り込みが可能。**R10 は対応**（0x3801 指定で JPEG のみ 578件、全体は 1160件）。
- ObjectFormat: `0x3801`=EXIF/JPEG, `0xb108`=Canon CR3(RAW), `0x3001`=Association(フォルダ)。
- R10 のハンドルは `0x919057X1`=CR3 / `0x919057X2`=JPEG のようにペア。`0x90000000` 系上位ハンドルはフォルダ。

## ハマりどころ（重要な教訓）

- **クライアント GUID のバイト順**: Windows の GUID `{A1AC802F-50A9-493B-962A-EA6F0366FDD9}` はメモリ上「前半3フィールドが LE、後半8バイトはそのまま」の混在形式。EOS Utility(Windows) はこの並びで送るのでカメラもこの並びで記憶する。全ビッグエンディアンで送ると無応答/INIT_FAIL になる。現行の既定 GUID (ワイヤ順) = `2f 80 ac a1 a9 50 3b 49 96 2a ea 6f 03 66 fd d9`。
  - 注意: `\Device\NPF_{...}` は **Npcap のキャプチャ用アダプタ ID** であり PTP/IP の GUID とは無関係。混同しないこと。
- **`TcpSocket` はムーブ専用**: デストラクタで `closesocket` するため、コピーするとハンドルが二重 close されて接続が壊れる（症状: ハンドシェイク成功後、最初の送信で `send failed`）。ムーブ実装＋コピー削除で対処済み。
- **`GetObject` はフォルダに効かない**: association ハンドルに投げると RC=0x2019。`GetObjectInfo` で実ファイルを判定してから取得する。
- **INIT_FAIL の切り分け**: カメラが「登録済みPCを探す」モードだと未登録 GUID を拒否（画面「接続先が見つかりません」）。新規 GUID を使う場合はカメラをペアリング待ち状態にし、初回接続時にカメラ画面で承認する。
- カメラが無応答のときに備え command ソケットに受信タイムアウト(10s)を設定している。

## 既知の制約 / 次の拡張候補

- `list()` は全ハンドルに `GetObjectInfo` を発行するため件数に比例して遅い。
- `get()` はオブジェクト全体をメモリに載せる（数十MBのCR3は問題なし）。
- 拡張候補: 全件保存、`GetPartialObject(0x101B)` によるチャンク転送＋進捗コールバック＋レジューム、`GetThumb(0x100A)` でサムネイル一覧、event チャネル監視による撮影即転送。
