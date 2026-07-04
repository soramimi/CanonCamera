// ptpip_prototype.cpp
//
// Canon EOS (PTP/IP over Wi-Fi) の最小プロトタイプ。
// 標準 PTP オペレーションで OpenSession -> GetStorageIDs ->
// GetObjectHandles -> GetObject（ダウンロード）まで通すことを目標にする。
//
// ビルド (MSVC):
//   cl /EHsc /std:c++17 ptpip_prototype.cpp ws2_32.lib
// ビルド (MinGW):
//   g++ -std=c++17 ptpip_prototype.cpp -lws2_32 -o ptpip_prototype
//
// 使い方:
//   ptpip_prototype <camera-ip>
//
// 前提: カメラを「EOS Utility」モードにして同じ LAN に載せる。
//       カメラ側 IP は液晶で確認できる。TCP/15740 に接続する。
//
// 注意: Canon EOS は本格的な制御に独自オペコード(0x9xxx)と
//       EOS_SetRemoteMode / EOS_SetEventMode の初期化が必要な機種が多い。
//       まずは標準 PTP で疎通確認し、必要に応じて Canon 拡張を足していく。
//       Canon 拡張の一覧は gphoto2 の camlibs/ptp2/ptp.h が実装リファレンス。

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

// ---- PTP/IP パケット種別 ----------------------------------------------------
enum PtpipType : uint32_t {
	PTPIP_INIT_COMMAND_REQUEST = 1,
	PTPIP_INIT_COMMAND_ACK = 2,
	PTPIP_INIT_EVENT_REQUEST = 3,
	PTPIP_INIT_EVENT_ACK = 4,
	PTPIP_INIT_FAIL = 5,
	PTPIP_CMD_REQUEST = 6,
	PTPIP_CMD_RESPONSE = 7,
	PTPIP_EVENT = 8,
	PTPIP_START_DATA_PACKET = 9,
	PTPIP_DATA_PACKET = 10,
	PTPIP_CANCEL_TRANSACTION = 11,
	PTPIP_END_DATA_PACKET = 12,
	PTPIP_PING = 13,
	PTPIP_PONG = 14,
};

// ---- 標準 PTP オペレーションコード -----------------------------------------
enum PtpOp : uint16_t {
	PTP_OP_OpenSession = 0x1002,
	PTP_OP_CloseSession = 0x1003,
	PTP_OP_GetStorageIDs = 0x1004,
	PTP_OP_GetObjectHandles = 0x1007,
	PTP_OP_GetObjectInfo = 0x1008,
	PTP_OP_GetObject = 0x1009,
	PTP_OP_GetThumb = 0x100A,
};

static uint16_t const PTP_RC_OK = 0x2001;

// -----------------------------------------------------------------------------
// バイトバッファへのリトルエンディアン書き込みヘルパ
// -----------------------------------------------------------------------------
static void put_u16(std::vector<uint8_t> &b, uint16_t v)
{
	b.push_back(v & 0xff);
	b.push_back((v >> 8) & 0xff);
}
static void put_u32(std::vector<uint8_t> &b, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		b.push_back((v >> (8 * i)) & 0xff);
}
static uint16_t get_u16(uint8_t const *p) { return p[0] | (p[1] << 8); }
static uint32_t get_u32(uint8_t const *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// UTF-16LE の null 終端文字列を書き込む（PTP/IP の Friendly Name 用）
static void put_utf16(std::vector<uint8_t> &b, std::string const &s)
{
	for (char c : s) {
		b.push_back((uint8_t)c);
		b.push_back(0);
	}
	b.push_back(0);
	b.push_back(0); // null terminator
}

// -----------------------------------------------------------------------------
// ソケットラッパ
// -----------------------------------------------------------------------------
class TcpSocket {
public:
	explicit TcpSocket(SOCKET s)
		: sock_(s)
	{
	}
	~TcpSocket()
	{
		if (sock_ != INVALID_SOCKET) closesocket(sock_);
	}

	// ムーブのみ許可。コピーするとハンドルの二重 close で接続が壊れる。
	TcpSocket(TcpSocket &&o) noexcept
		: sock_(o.sock_)
	{
		o.sock_ = INVALID_SOCKET;
	}
	TcpSocket &operator=(TcpSocket &&o) noexcept
	{
		if (this != &o) {
			if (sock_ != INVALID_SOCKET) closesocket(sock_);
			sock_ = o.sock_;
			o.sock_ = INVALID_SOCKET;
		}
		return *this;
	}
	TcpSocket(TcpSocket const &) = delete;
	TcpSocket &operator=(TcpSocket const &) = delete;

	static TcpSocket connect(std::string const &ip, uint16_t port)
	{
		addrinfo hints {}, *res = nullptr;
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		std::string ports = std::to_string(port);
		if (getaddrinfo(ip.c_str(), ports.c_str(), &hints, &res) != 0)
			throw std::runtime_error("getaddrinfo failed");
		SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
		if (s == INVALID_SOCKET) {
			freeaddrinfo(res);
			throw std::runtime_error("socket() failed");
		}
		if (::connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) {
			freeaddrinfo(res);
			closesocket(s);
			throw std::runtime_error("connect() failed: " + std::to_string(WSAGetLastError()));
		}
		freeaddrinfo(res);
		return TcpSocket(s);
	}

	// 受信タイムアウト（ミリ秒）。0 で無制限。
	void set_recv_timeout(int ms)
	{
		DWORD t = (DWORD)ms;
		setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, (char const *)&t, sizeof(t));
	}

	void send_all(std::vector<uint8_t> const &data)
	{
		size_t off = 0;
		while (off < data.size()) {
			int n = ::send(sock_, (char const *)data.data() + off, (int)(data.size() - off), 0);
			if (n <= 0)
				throw std::runtime_error("send failed: " + std::to_string(WSAGetLastError()));
			off += n;
		}
	}

	void recv_exact(uint8_t *buf, size_t len)
	{
		size_t off = 0;
		while (off < len) {
			int n = ::recv(sock_, (char *)buf + off, (int)(len - off), 0);
			if (n == 0) throw std::runtime_error("connection closed by peer");
			if (n < 0) {
				int e = WSAGetLastError();
				if (e == WSAETIMEDOUT)
					throw std::runtime_error("recv timeout (no reply from camera)");
				throw std::runtime_error("recv failed: " + std::to_string(e));
			}
			off += n;
		}
	}

private:
	SOCKET sock_;
};

// -----------------------------------------------------------------------------
// PTP/IP フレーミング: [length(4)][type(4)][payload...]
// -----------------------------------------------------------------------------
struct Packet {
	uint32_t type;
	std::vector<uint8_t> payload;
};

static void send_packet(TcpSocket &s, uint32_t type, std::vector<uint8_t> const &payload)
{
	std::vector<uint8_t> pkt;
	uint32_t len = 8 + (uint32_t)payload.size();
	put_u32(pkt, len);
	put_u32(pkt, type);
	pkt.insert(pkt.end(), payload.begin(), payload.end());
	s.send_all(pkt);
}

static Packet recv_packet(TcpSocket &s)
{
	uint8_t hdr[8];
	s.recv_exact(hdr, 8);
	uint32_t len = get_u32(hdr);
	uint32_t type = get_u32(hdr + 4);
	if (len < 8) throw std::runtime_error("invalid packet length");
	Packet p;
	p.type = type;
	p.payload.resize(len - 8);
	if (!p.payload.empty()) s.recv_exact(p.payload.data(), p.payload.size());
	return p;
}

// -----------------------------------------------------------------------------
// PTP/IP 接続確立（2 本のコネクション: command + event）
// -----------------------------------------------------------------------------
struct PtpipConnection {
	TcpSocket cmd;
	TcpSocket evt;
	uint32_t transaction = 0;

	uint32_t next_tid() { return ++transaction; }
};

// 本物の EOS Utility が Init Command Request で送っている 16 バイトの
// クライアント GUID を、Wireshark のキャプチャからワイヤ上のバイト順のまま
// ここに貼る。探し方:
//   フィルタ tcp.port == 15740 → PC→カメラの最初のパケット →
//   先頭 8 バイト(length4 + type[01 00 00 00])の直後の 16 バイト。
// ※ ワイヤ上のバイトなのでバイト順変換は不要。そのまま並べる。
static uint8_t const kClientGuid[16] = {
	// TODO: Wireshark で観測した実際の 16 バイトに置き換える
	// 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	// 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	// 2f80aca1a9503b49962aea6f0366fdd9
	0x2f,
	0x80,
	0xac,
	0xa1,
	0xa9,
	0x50,
	0x3b,
	0x49,
	0x96,
	0x2a,
	0xea,
	0x6f,
	0x03,
	0x66,
	0xfd,
	0xd9,

};

static PtpipConnection ptpip_connect(std::string const &ip)
{
	TcpSocket cmd = TcpSocket::connect(ip, 15740);
	printf("[+] TCP connected (command channel).\n");
	// Ack を無制限に待たず、10 秒で切る。無制限で待ちたいなら 0 に。
	cmd.set_recv_timeout(10000);

	// 1) Init Command Request: GUID + FriendlyName(UTF-16) + ProtocolVersion
	printf("[i] GUID bytes:");
	for (int i = 0; i < 16; i++)
		printf(" %02x", kClientGuid[i]);
	printf("\n");
	{
		std::vector<uint8_t> pl;
		pl.insert(pl.end(), kClientGuid, kClientGuid + 16);
		put_utf16(pl, "ptpip-prototype");
		put_u32(pl, 0x00010000); // protocol version
		send_packet(cmd, PTPIP_INIT_COMMAND_REQUEST, pl);
	}
	printf("[+] Init Command Request sent (%d bytes GUID). waiting for Ack...\n", 16);
	printf("    ※ カメラ画面に接続確認ダイアログが出ていないか確認してください。\n");

	// 2) Init Command Ack を受信 -> ConnectionNumber を取り出す
	Packet ack = recv_packet(cmd);
	if (ack.type == PTPIP_INIT_FAIL) {
		// INIT_FAIL の payload 先頭 4 バイトが reason code
		uint32_t reason = ack.payload.size() >= 4 ? get_u32(ack.payload.data()) : 0;
		char msg[128];
		snprintf(msg, sizeof(msg),
			"camera rejected connection (INIT_FAIL), reason=0x%08x", reason);
		throw std::runtime_error(msg);
	}
	if (ack.type != PTPIP_INIT_COMMAND_ACK)
		throw std::runtime_error("unexpected packet: " + std::to_string(ack.type));
	uint32_t connNumber = get_u32(ack.payload.data());
	printf("[+] Init Command Ack. connection number = %u\n", connNumber);

	// 3) 別ソケットで Event チャネルを張る
	TcpSocket evt = TcpSocket::connect(ip, 15740);
	{
		std::vector<uint8_t> pl;
		put_u32(pl, connNumber);
		send_packet(evt, PTPIP_INIT_EVENT_REQUEST, pl);
	}
	Packet evtAck = recv_packet(evt);
	if (evtAck.type != PTPIP_INIT_EVENT_ACK)
		throw std::runtime_error("event init failed");
	printf("[+] Event channel established.\n");

	return PtpipConnection { std::move(cmd), std::move(evt) };
}

// -----------------------------------------------------------------------------
// 1 回のオペレーション実行
//   dataOut : 初期化側 -> カメラへ送るデータ（無ければ空）
//   dataIn  : カメラ -> 初期化側で受け取ったデータ（GetObject 等）
// 戻り値    : レスポンスコード（0x2001 が OK）
// -----------------------------------------------------------------------------
static uint16_t ptp_transaction(
	PtpipConnection &c,
	uint16_t opcode,
	std::vector<uint32_t> params,
	std::vector<uint8_t> *dataIn = nullptr,
	std::vector<uint8_t> const *dataOut = nullptr)
{
	uint32_t tid = c.next_tid();

	// DataPhaseInfo: 1 = data-in / no data, 2 = data-out（gphoto の実装に合わせて要検証）
	uint32_t dataphase = (dataOut && !dataOut->empty()) ? 2 : 1;

	// ---- Operation Request Packet ----
	{
		std::vector<uint8_t> pl;
		put_u32(pl, dataphase);
		put_u16(pl, opcode);
		put_u32(pl, tid);
		for (auto p : params)
			put_u32(pl, p);
		send_packet(c.cmd, PTPIP_CMD_REQUEST, pl);
	}

	// ---- data-out フェーズ（今回のダウンロード用途では未使用）----
	if (dataOut && !dataOut->empty()) {
		// Start Data Packet: transactionID(4) + totalLength(8)
		std::vector<uint8_t> sp;
		put_u32(sp, tid);
		put_u32(sp, (uint32_t)dataOut->size());
		put_u32(sp, 0);
		send_packet(c.cmd, PTPIP_START_DATA_PACKET, sp);
		// End Data Packet: transactionID(4) + data
		std::vector<uint8_t> ep;
		put_u32(ep, tid);
		ep.insert(ep.end(), dataOut->begin(), dataOut->end());
		send_packet(c.cmd, PTPIP_END_DATA_PACKET, ep);
	}

	// ---- data-in フェーズ + レスポンス受信 ----
	// START_DATA -> DATA... -> END_DATA -> CMD_RESPONSE の順に来る。
	for (;;) {
		Packet p = recv_packet(c.cmd);
		switch (p.type) {
		case PTPIP_START_DATA_PACKET:
			// payload: transactionID(4) + totalLength(8)。ここでは長さは使わない。
			if (dataIn) dataIn->clear();
			break;
		case PTPIP_DATA_PACKET:
		case PTPIP_END_DATA_PACKET:
			// payload: transactionID(4) + data chunk
			if (dataIn && p.payload.size() > 4)
				dataIn->insert(dataIn->end(), p.payload.begin() + 4, p.payload.end());
			break;
		case PTPIP_CMD_RESPONSE: {
			uint16_t rc = get_u16(p.payload.data());
			return rc;
		}
		case PTPIP_EVENT:
			// イベントは本来 event チャネルに来るが、無視して読み飛ばす
			break;
		default:
			throw std::runtime_error("unexpected packet in transaction: " + std::to_string(p.type));
		}
	}
}

// -----------------------------------------------------------------------------
// PTP の配列型 (AUINT32): [count(4)][elem0(4)][elem1(4)]...
// -----------------------------------------------------------------------------
static std::vector<uint32_t> parse_u32_array(std::vector<uint8_t> const &d)
{
	std::vector<uint32_t> out;
	if (d.size() < 4) return out;
	uint32_t n = get_u32(d.data());
	for (uint32_t i = 0; i < n && 4 + i * 4 + 4 <= d.size(); i++)
		out.push_back(get_u32(d.data() + 4 + i * 4));
	return out;
}

// -----------------------------------------------------------------------------
// GetObjectInfo のデータセットから必要な項目を取り出す。
//   レイアウト(先頭からの固定オフセット):
//     0  StorageID(u32)
//     4  ObjectFormat(u16)     0x3001=Association(フォルダ), 0x3801=EXIF/JPEG など
//     6  ProtectionStatus(u16)
//     8  ObjectCompressedSize(u32)
//     ...（Thumb/画像サイズ等）...
//     42 AssociationType(u16)   0以外ならフォルダ
//     ...
//     52 Filename(PTPString: [文字数(u8)][UCS-2LE...])
// -----------------------------------------------------------------------------
struct ObjectInfo {
	uint16_t format = 0;
	uint32_t size = 0;
	uint16_t assocType = 0;
	std::string filename;
};

static ObjectInfo parse_object_info(std::vector<uint8_t> const &d)
{
	ObjectInfo oi;
	if (d.size() < 52) return oi;
	oi.format = get_u16(d.data() + 4);
	oi.size = get_u32(d.data() + 8);
	oi.assocType = get_u16(d.data() + 42);
	// Filename: オフセット52。先頭 u8 が文字数(終端含む)、以降 UCS-2LE。
	size_t off = 52;
	uint8_t nchars = d[off++];
	for (uint8_t i = 0; i < nchars && off + 1 < d.size(); i++, off += 2) {
		uint16_t ch = d[off] | (d[off + 1] << 8);
		if (ch == 0) break;
		oi.filename.push_back(ch < 128 ? (char)ch : '_'); // ASCII のみ簡易表示
	}
	return oi;
}

static bool is_association(ObjectInfo const &oi)
{
	return oi.format == 0x3001 || oi.assocType != 0;
}

// -----------------------------------------------------------------------------
int main(int argc, char **argv)
{
	if (argc < 2) {
		printf("usage: %s <camera-ip>\n", argv[0]);
		return 1;
	}
	std::string const ip = argv[1];

	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		printf("WSAStartup failed\n");
		return 1;
	}

	try {
		printf("[*] connecting to %s:15740 ...\n", ip.c_str());
		PtpipConnection c = ptpip_connect(ip);

		// OpenSession（SessionID は 1 固定でよい）
		uint16_t rc = ptp_transaction(c, PTP_OP_OpenSession, { 1 });
		printf("[+] OpenSession rc=0x%04x\n", rc);

		// GetStorageIDs
		std::vector<uint8_t> data;
		rc = ptp_transaction(c, PTP_OP_GetStorageIDs, {}, &data);
		auto storages = parse_u32_array(data);
		printf("[+] GetStorageIDs rc=0x%04x, %zu storage(s)\n", rc, storages.size());

		// 各ストレージのオブジェクトハンドル一覧を取得
		for (uint32_t sid : storages) {
			data.clear();
			// params: storageID, objectFormatCode(0=all), associationHandle(0)
			rc = ptp_transaction(c, PTP_OP_GetObjectHandles, { sid, 0, 0 }, &data);
			auto handles = parse_u32_array(data);
			printf("[+] storage 0x%08x: %zu object(s)\n", sid, handles.size());

			// 各ハンドルの ObjectInfo を見て、実ファイル(非フォルダ)を探す。
			// 最初に見つかった画像ファイルを 1 つダウンロードする。
			int shown = 0;
			bool downloaded = false;
			for (uint32_t h : handles) {
				std::vector<uint8_t> info;
				uint16_t irc = ptp_transaction(c, PTP_OP_GetObjectInfo, { h }, &info);
				if (irc != PTP_RC_OK) continue;
				ObjectInfo oi = parse_object_info(info);

				if (is_association(oi)) continue; // フォルダは飛ばす

				if (shown < 20) {
					printf("    handle=0x%08x fmt=0x%04x %8u bytes  %s\n",
						h, oi.format, oi.size, oi.filename.c_str());
					shown++;
				}

				if (!downloaded) {
					std::vector<uint8_t> obj;
					uint16_t grc = ptp_transaction(c, PTP_OP_GetObject, { h }, &obj);
					printf("[+] GetObject handle=0x%08x rc=0x%04x, %zu bytes\n",
						h, grc, obj.size());
					if (grc == PTP_RC_OK && !obj.empty()) {
						std::string name = oi.filename.empty() ? "downloaded.bin" : oi.filename;
						FILE *f = fopen(name.c_str(), "wb");
						if (f) {
							fwrite(obj.data(), 1, obj.size(), f);
							fclose(f);
						}
						printf("    -> saved to %s\n", name.c_str());
						downloaded = true;
					}
				}
				if (downloaded && shown >= 20) break;
			}
		}

		ptp_transaction(c, PTP_OP_CloseSession, {});
		printf("[*] done.\n");
	} catch (std::exception const &e) {
		printf("[!] error: %s\n", e.what());
		WSACleanup();
		return 1;
	}

	WSACleanup();
	return 0;
}
