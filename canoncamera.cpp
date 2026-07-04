// canoncamera.cpp
//
// CanonCamera の実装。PTP/IP のフレーミング・ハンドシェイク・オペレーションは
// すべてこのファイル内に閉じ込める（ptpip_prototype.cpp のロジックを整理したもの）。

#include "canoncamera.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

namespace {

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

uint16_t const PTP_RC_OK = 0x2001;

// ---- LE 読み書きヘルパ ------------------------------------------------------
void put_u16(std::vector<uint8_t> &b, uint16_t v)
{
	b.push_back(v & 0xff);
	b.push_back((v >> 8) & 0xff);
}
void put_u32(std::vector<uint8_t> &b, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		b.push_back((v >> (8 * i)) & 0xff);
}
uint16_t get_u16(uint8_t const *p) { return p[0] | (p[1] << 8); }
uint32_t get_u32(uint8_t const *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
void put_utf16(std::vector<uint8_t> &b, std::string const &s)
{
	for (char c : s) {
		b.push_back((uint8_t)c);
		b.push_back(0);
	}
	b.push_back(0);
	b.push_back(0);
}

// ---- ソケット（ムーブ専用） -------------------------------------------------
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
			if (n <= 0) throw std::runtime_error("send failed: " + std::to_string(WSAGetLastError()));
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
				if (e == WSAETIMEDOUT) throw std::runtime_error("recv timeout (no reply from camera)");
				throw std::runtime_error("recv failed: " + std::to_string(e));
			}
			off += n;
		}
	}

private:
	SOCKET sock_;
};

// ---- PTP/IP フレーミング: [length(4)][type(4)][payload...] -------------------
struct Packet {
	uint32_t type;
	std::vector<uint8_t> payload;
};

void send_packet(TcpSocket &s, uint32_t type, std::vector<uint8_t> const &payload)
{
	std::vector<uint8_t> pkt;
	put_u32(pkt, 8 + (uint32_t)payload.size());
	put_u32(pkt, type);
	pkt.insert(pkt.end(), payload.begin(), payload.end());
	s.send_all(pkt);
}

Packet recv_packet(TcpSocket &s)
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

std::vector<uint32_t> parse_u32_array(std::vector<uint8_t> const &d)
{
	std::vector<uint32_t> out;
	if (d.size() < 4) return out;
	uint32_t n = get_u32(d.data());
	for (uint32_t i = 0; i < n && 4 + i * 4 + 4 <= d.size(); i++)
		out.push_back(get_u32(d.data() + 4 + i * 4));
	return out;
}

// GetObjectInfo データセットから必要項目を取り出す（レイアウトは PTP 仕様の固定オフセット）。
struct ObjectInfo {
	uint32_t storageId = 0;
	uint16_t format = 0;
	uint32_t size = 0;
	uint16_t assocType = 0;
	std::string filename;
};

ObjectInfo parse_object_info(std::vector<uint8_t> const &d)
{
	ObjectInfo oi;
	if (d.size() < 52) return oi;
	oi.storageId = get_u32(d.data() + 0);
	oi.format = get_u16(d.data() + 4);
	oi.size = get_u32(d.data() + 8);
	oi.assocType = get_u16(d.data() + 42);
	size_t off = 52; // Filename: [文字数(u8)][UCS-2LE...]
	uint8_t nchars = d[off++];
	for (uint8_t i = 0; i < nchars && off + 1 < d.size(); i++, off += 2) {
		uint16_t ch = d[off] | (d[off + 1] << 8);
		if (ch == 0) break;
		oi.filename.push_back(ch < 128 ? (char)ch : '_');
	}
	return oi;
}

bool is_association(ObjectInfo const &oi)
{
	return oi.format == 0x3001 || oi.assocType != 0;
}

// EOS Utility が使う既定のクライアント GUID（ワイヤ上のバイト順）。
uint8_t const kDefaultGuid[16] = {
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

} // anonymous namespace

// -----------------------------------------------------------------------------
// Impl: 接続状態とトランザクション実行を保持する。
// -----------------------------------------------------------------------------
struct CanonCamera::Impl {
	bool wsaStarted = false;
	std::unique_ptr<TcpSocket> cmd;
	std::unique_ptr<TcpSocket> evt;
	uint32_t transaction = 0;
	uint8_t guid[16];

	Impl() { memcpy(guid, kDefaultGuid, 16); }
	~Impl()
	{
		if (wsaStarted) WSACleanup();
	}

	uint32_t next_tid() { return ++transaction; }

	// 1 オペレーションを実行し、レスポンスコードを返す。dataIn/dataOut は任意。
	uint16_t transact(uint16_t opcode,
		std::vector<uint32_t> params,
		std::vector<uint8_t> *dataIn = nullptr,
		std::vector<uint8_t> const *dataOut = nullptr)
	{
		uint32_t tid = next_tid();
		uint32_t dataphase = (dataOut && !dataOut->empty()) ? 2 : 1;

		std::vector<uint8_t> pl;
		put_u32(pl, dataphase);
		put_u16(pl, opcode);
		put_u32(pl, tid);
		for (auto p : params)
			put_u32(pl, p);
		send_packet(*cmd, PTPIP_CMD_REQUEST, pl);

		if (dataOut && !dataOut->empty()) {
			std::vector<uint8_t> sp;
			put_u32(sp, tid);
			put_u32(sp, (uint32_t)dataOut->size());
			put_u32(sp, 0);
			send_packet(*cmd, PTPIP_START_DATA_PACKET, sp);
			std::vector<uint8_t> ep;
			put_u32(ep, tid);
			ep.insert(ep.end(), dataOut->begin(), dataOut->end());
			send_packet(*cmd, PTPIP_END_DATA_PACKET, ep);
		}

		for (;;) {
			Packet p = recv_packet(*cmd);
			switch (p.type) {
			case PTPIP_START_DATA_PACKET:
				if (dataIn) dataIn->clear();
				break;
			case PTPIP_DATA_PACKET:
			case PTPIP_END_DATA_PACKET:
				if (dataIn && p.payload.size() > 4)
					dataIn->insert(dataIn->end(), p.payload.begin() + 4, p.payload.end());
				break;
			case PTPIP_CMD_RESPONSE:
				return p.payload.size() >= 2 ? get_u16(p.payload.data()) : 0;
			case PTPIP_EVENT:
				break; // command チャネルに来たイベントは無視
			default:
				throw std::runtime_error("unexpected packet: " + std::to_string(p.type));
			}
		}
	}

	void connect(std::string const &ip)
	{
		WSADATA wsa;
		if (!wsaStarted) {
			if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
				throw std::runtime_error("WSAStartup failed");
			wsaStarted = true;
		}

		// command チャネル
		cmd.reset(new TcpSocket(TcpSocket::connect(ip, 15740)));
		cmd->set_recv_timeout(10000);

		std::vector<uint8_t> pl;
		pl.insert(pl.end(), guid, guid + 16);
		put_utf16(pl, "CanonCamera");
		put_u32(pl, 0x00010000); // protocol version
		send_packet(*cmd, PTPIP_INIT_COMMAND_REQUEST, pl);

		Packet ack = recv_packet(*cmd);
		if (ack.type == PTPIP_INIT_FAIL) {
			uint32_t reason = ack.payload.size() >= 4 ? get_u32(ack.payload.data()) : 0;
			char msg[96];
			snprintf(msg, sizeof(msg), "INIT_FAIL (reason=0x%08x)", reason);
			throw std::runtime_error(msg);
		}
		if (ack.type != PTPIP_INIT_COMMAND_ACK)
			throw std::runtime_error("unexpected packet during init");
		uint32_t connNumber = get_u32(ack.payload.data());

		// event チャネル
		evt.reset(new TcpSocket(TcpSocket::connect(ip, 15740)));
		std::vector<uint8_t> ev;
		put_u32(ev, connNumber);
		send_packet(*evt, PTPIP_INIT_EVENT_REQUEST, ev);
		if (recv_packet(*evt).type != PTPIP_INIT_EVENT_ACK)
			throw std::runtime_error("event channel init failed");
	}
};

// -----------------------------------------------------------------------------
// 公開 API
// -----------------------------------------------------------------------------
CanonCamera::CanonCamera()
	: impl_(new Impl)
{
}
CanonCamera::~CanonCamera() { close(); }

bool CanonCamera::fail(std::string const &msg)
{
	error_ = msg;
	return false;
}

bool CanonCamera::isOpen() const { return impl_ && impl_->cmd; }

void CanonCamera::setClientGuid(uint8_t const guid[16])
{
	memcpy(impl_->guid, guid, 16);
}

bool CanonCamera::open(std::string const &ip)
{
	try {
		impl_->connect(ip);
		uint16_t rc = impl_->transact(PTP_OP_OpenSession, { 1 });
		if (rc != PTP_RC_OK) {
			close();
			char m[64];
			snprintf(m, sizeof(m), "OpenSession failed rc=0x%04x", rc);
			return fail(m);
		}
		return true;
	} catch (std::exception const &e) {
		close();
		return fail(e.what());
	}
}

void CanonCamera::close()
{
	if (impl_ && impl_->cmd) {
		try {
			impl_->transact(PTP_OP_CloseSession, {});
		} catch (...) {
		}
	}
	if (impl_) {
		impl_->cmd.reset();
		impl_->evt.reset();
		impl_->transaction = 0;
	}
}

// filter に一致する Item かを判定する（クライアント側の最終フィルタ）。
static bool matches_filter(CanonCamera::Filter f, CanonCamera::Item const &it)
{
	switch (f) {
	case CanonCamera::Filter::Jpeg: return it.isJpeg();
	case CanonCamera::Filter::Raw: return it.isRaw();
	case CanonCamera::Filter::All:
	default: return true;
	}
}

std::vector<CanonCamera::Item> CanonCamera::list(Filter filter)
{
	std::vector<Item> items;
	if (!isOpen()) {
		fail("not open");
		return items;
	}

	// カメラ側フォーマット絞り込みに渡すコード（0 = 全フォーマット）。
	// JPEG は単一コードなのでカメラ側で絞れる。RAW は複数コードになり得るため
	// カメラ側は絞らずクライアント側判定に任せる。
	uint32_t formatCode = (filter == Filter::Jpeg) ? 0x3801 : 0;

	try {
		std::vector<uint8_t> data;
		if (impl_->transact(PTP_OP_GetStorageIDs, {}, &data) != PTP_RC_OK) {
			fail("GetStorageIDs failed");
			return items;
		}
		auto storages = parse_u32_array(data);

		for (uint32_t sid : storages) {
			data.clear();
			// params: storageID, objectFormatCode, associationHandle(0=全体)
			if (impl_->transact(PTP_OP_GetObjectHandles, { sid, formatCode, 0 }, &data) != PTP_RC_OK)
				continue;
			auto handles = parse_u32_array(data);

			for (uint32_t h : handles) {
				std::vector<uint8_t> info;
				if (impl_->transact(PTP_OP_GetObjectInfo, { h }, &info) != PTP_RC_OK)
					continue;
				ObjectInfo oi = parse_object_info(info);
				if (is_association(oi)) continue; // フォルダは除外

				Item it;
				it.handle = h;
				it.storageId = oi.storageId ? oi.storageId : sid;
				it.format = oi.format;
				it.size = oi.size;
				it.filename = oi.filename;

				// カメラがフォーマット絞り込みを無視しても正しく効くよう最終判定。
				if (!matches_filter(filter, it)) continue;

				items.push_back(std::move(it));
			}
		}
		return items;
	} catch (std::exception const &e) {
		fail(e.what());
		return items;
	}
}

bool CanonCamera::get(Item const &item, char const *savepath)
{
	if (!isOpen()) return fail("not open");
	try {
		std::vector<uint8_t> obj;
		uint16_t rc = impl_->transact(PTP_OP_GetObject, { item.handle }, &obj);
		if (rc != PTP_RC_OK) {
			char m[64];
			snprintf(m, sizeof(m), "GetObject failed rc=0x%04x", rc);
			return fail(m);
		}
		FILE *f = fopen(savepath, "wb");
		if (!f) return fail(std::string("cannot open file: ") + savepath);
		size_t wrote = obj.empty() ? 0 : fwrite(obj.data(), 1, obj.size(), f);
		fclose(f);
		if (wrote != obj.size()) return fail("write incomplete");
		// ObjectInfo のサイズが分かっている場合は検証
		if (item.size && obj.size() != item.size)
			return fail("size mismatch (expected " + std::to_string(item.size) + ", got " + std::to_string(obj.size()) + ")");
		return true;
	} catch (std::exception const &e) {
		return fail(e.what());
	}
}
