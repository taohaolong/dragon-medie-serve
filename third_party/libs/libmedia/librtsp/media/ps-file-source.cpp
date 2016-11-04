#include "ps-file-source.h"
#include "cstringext.h"
#include "mpeg-ps.h"
#include "rtp-profile.h"
#include "../payload/rtp-pack.h"
#include <assert.h>

#define MAX_UDP_PACKET (1450-16)

extern "C" int rtp_ssrc(void);

PSFileSource::PSFileSource(const char *file)
:m_reader(file)
{
	m_speed = 1.0;
	m_status = 0;
	m_ps_clock = 0;
	m_rtp_clock = 0;
	m_rtcp_clock = 0;

	unsigned int ssrc = (unsigned int)rtp_ssrc();

	struct mpeg_ps_func_t func;
	func.alloc = Alloc;
	func.free = Free;
	func.write = Packet;
	m_ps = mpeg_ps_create(&func, this);
	mpeg_ps_add_stream(m_ps, STREAM_VIDEO_H264, NULL, 0);

	static struct rtp_pack_func_t s_psfunc = {
		PSFileSource::RTPAlloc,
		PSFileSource::RTPFree,
		PSFileSource::RTPPacket,
	};
	m_pspacker = rtp_ps_packer()->create(ssrc, (unsigned short)ssrc, RTP_PAYLOAD_MPEG2PS, &s_psfunc, this);

	struct rtp_event_t event;
	event.on_rtcp = OnRTCPEvent;
	m_rtp = rtp_create(&event, this, ssrc, 90000, 4*1024);
	rtp_set_info(m_rtp, "RTSPServer", "szj.h264");
}

PSFileSource::~PSFileSource()
{
	if(m_rtp)
		rtp_destroy(m_rtp);
	if(m_pspacker)
		rtp_ps_packer()->destroy(m_pspacker);
	mpeg_ps_destroy(m_ps);
}

int PSFileSource::SetRTPSocket(const char* ip, socket_t socket[2], unsigned short port[2])
{
	int r1 = socket_addr_from(&m_addr[0], &m_addrlen[0], ip, port[0]);
	int r2 = socket_addr_from(&m_addr[1], &m_addrlen[1], ip, port[1]);
	if (0 != r1 || 0 != r2)
		return 0 != r1 ? r1 : r2;

	m_socket[0] = socket[0];
	m_socket[1] = socket[1];
	return 0;
}

int PSFileSource::Play()
{
	m_status = 1;

	time64_t clock = time64_now();
	if(0 == m_rtp_clock || m_rtp_clock + 40 < clock)
	{
		void* ptr = NULL;
		size_t bytes = 0;
		if(0 == m_reader.GetNextFrame(m_pos, ptr, bytes))
		{
			if(0 == m_ps_clock)
				m_ps_clock = clock;
			mpeg_ps_write(m_ps, STREAM_VIDEO_H264, (clock-m_ps_clock)*90, (clock-m_ps_clock)*90, ptr, bytes);
			m_rtp_clock = clock;

			SendRTCP();
			return 1;
		}
	}

	return 0;
}

int PSFileSource::Pause()
{
	m_status = 2;
	return 0;
}

int PSFileSource::Seek(int64_t pos)
{
	return m_reader.Seek(pos);
}

int PSFileSource::SetSpeed(double speed)
{
	m_speed = speed;
	return 0;
}

int PSFileSource::GetDuration(int64_t& duration) const
{
	return m_reader.GetDuration(duration);
}

int PSFileSource::GetSDPMedia(std::string& sdp) const
{
	static const char* pattern =
		"m=video 0 RTP/AVP %d\n"
		"a=rtpmap:%d MP2P/90000\n";
	
	char media[64];
	snprintf(media, sizeof(media), pattern, RTP_PAYLOAD_MPEG2PS, RTP_PAYLOAD_MPEG2PS);
	sdp = media;
	return 0;
}

int PSFileSource::GetRTPInfo(int64_t &pos, unsigned short &seq, unsigned int &rtptime) const
{
	rtp_h264_packer()->get_info(m_pspacker, &seq, &rtptime);
	pos = m_pos;
	return 0;
}

void PSFileSource::OnRTCPEvent(const struct rtcp_msg_t* msg)
{
	msg;
}

void PSFileSource::OnRTCPEvent(void* param, const struct rtcp_msg_t* msg)
{
	PSFileSource *self = (PSFileSource *)param;
	self->OnRTCPEvent(msg);
}

int PSFileSource::SendRTCP()
{
	// make sure have sent RTP packet

	time64_t clock = time64_now();
	int interval = rtp_rtcp_interval(m_rtp);
	if(0 == m_rtcp_clock || m_rtcp_clock + interval < clock)
	{
		char rtcp[1024] = {0};
		size_t n = rtp_rtcp_report(m_rtp, rtcp, sizeof(rtcp));

		// send RTCP packet
		socket_sendto(m_socket[1], rtcp, n, 0, (struct sockaddr*)&m_addr[1], m_addrlen[1]);

		m_rtcp_clock = clock;
	}

	return 0;
}

void* PSFileSource::Alloc(void* /*param*/, size_t bytes)
{
//	PSFileSource* self = (PSFileSource*)param;
	return malloc(bytes);
}

void PSFileSource::Free(void* /*param*/, void* packet)
{
//	PSFileSource* self = (PSFileSource*)param;
	return free(packet);
}

void PSFileSource::Packet(void* param, int /*avtype*/, void* pes, size_t bytes)
{
	PSFileSource* self = (PSFileSource*)param;
	time64_t clock = time64_now();
	rtp_ps_packer()->input(self->m_pspacker, pes, bytes, clock);
	free(pes);
}

void* PSFileSource::RTPAlloc(void* param, size_t bytes)
{
	PSFileSource *self = (PSFileSource*)param;
	assert(bytes <= sizeof(self->m_packet));
	return self->m_packet;
}

void PSFileSource::RTPFree(void* param, void *packet)
{
	PSFileSource *self = (PSFileSource*)param;
	assert(self->m_packet == packet);
}

void PSFileSource::RTPPacket(void* param, void *packet, size_t bytes, uint64_t time)
{
	PSFileSource *self = (PSFileSource*)param;
	assert(self->m_packet == packet);

	int r = socket_sendto(self->m_socket[0], packet, bytes, 0, (struct sockaddr*)&self->m_addr[0], self->m_addrlen[0]);
	assert(r == (int)bytes);
	rtp_onsend(self->m_rtp, packet, bytes/*, time*/);
}
