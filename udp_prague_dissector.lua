-- udpprague.lua

-- Port to which we attach the dissector (Change to your port, 8080 is the default in app_stuff.h)
local UDPPRAGUE_PORT = 8080

-- Plugin info
local udpprague_info =
{
	version = "1.1.0",
	author = "Yonah Thienpont, Chia-Yu Chang",
	description = "Dissector for UDP Prague",
	repository = "https://github.com/L4STeam/udp_prague"
}
set_plugin_info(udpprague_info)

-- Create a protocol to attach new fields to
local udpprague_p  = Proto.new("udpprague", "UdpPrague")
local f            = udpprague_p.fields

-- New types
local udpprague_t  = { [1]="Bulk sender", [2]="Real-Time sender", [17]="Per-pkt ACK receiver", [18]="RFC-8888 ACK receiver" }
local ipecn_t      = { [0]="Not ECN-Capable Transport", [1]="ECN-Capable Transport (1)", [2]="ECN-Capable Transport (0)", [3]="Congestion Experienced" }

-- ProtoField.new(name, abbr, type, [valuestring], [base], [mask], [description])
-- ProtoField.type(abbr, [name], [base], [valuestring], [mask], [description])
-- ProtoField.bytes(abbr, [name], [display], [description])
f.type        = ProtoField.uint8( "udpprague.type",        "UDP Prague Type",   base.DEC,  udpprague_t, nil, "UDP Prague packet type")

-- For Bulk data, Real-time data, and Per-pkt ACK
f.timestamp   = ProtoField.int32( "udpprague.ts",          "Timestamp",         base.DEC,  nil,         nil, "Timestamp")
f.echoed_ts   = ProtoField.int32( "udpprague.echo_ts",     "Echo Timestamp",    base.DEC,  nil,         nil, "Echoed timestamp")

-- For Bulk data, Real-time data
f.seq_nr      = ProtoField.int32( "udpprague.seq_nr",      "Sequence Number",   base.DEC,  nil,         nil, "Packet sequence number")

-- For Real-time data
f.frame_nr    = ProtoField.int32( "udpprague.frame_nr",    "Frame Number",      base.DEC,  nil,         nil, "Frame sequence number")
f.frame_sent  = ProtoField.int32( "udpprague.frame_sent",  "Frame Sent",        base.DEC,  nil,         nil, "Frame sent in bytes")
f.frame_size  = ProtoField.int32( "udpprague.frame_size",  "Frame Size",        base.DEC,  nil,         nil, "Frame size in bytes")

-- For Per-pkt ACK
f.ack_seq     = ProtoField.int32( "udpprague.ack_seq",     "Ack Sequence",      base.DEC,  nil,         nil, "Acked sequence number")
f.pkt_rcvd    = ProtoField.int32( "udpprague.pkts_revd",   "Packets Received",  base.DEC,  nil,         nil, "Packets received counter")
f.pkt_ce      = ProtoField.int32( "udpprague.pkts_ce",     "Packets CE",        base.DEC,  nil,         nil, "Packets CE-marked conuter")
f.pkt_lost    = ProtoField.int32( "udpprague.pkts_lost",   "Packets Lost",      base.DEC,  nil,         nil, "Packets lost counter")
f.error_l4s   = ProtoField.bool(  "udpprague.error_l4s",   "Error L4S",         base.NONE, nil,         nil, "Error flag")

-- For RFC-8888 ACK
f.rfc8888_seq = ProtoField.int32( "udpprague.rfc8888_seq", "RFC8888 Sequence",  base.DEC,  nil,         nil, "Start sequence in RFC8888 ACK")
f.rfc8888_num = ProtoField.uint16("udpprague.rfc8888_num", "RFC8888 Number",    base.DEC,  nil,         nil, "Report numbers in RFC8888 ACK")
f.rfc8888_rpt = ProtoField.uint16("udpprague.rfc8888_rpt", "RFC8888 Report",    base.DEC,  nil,         nil, "Report in RFC8888 ACK")

-- For each RFC-8888 report
f.rfc8888_rpt_rcv = ProtoField.uint16("udpprague.rfc8888_rpt_rcv", "RFC8888 Report receive flag",    base.DEC,  nil,     0x8000)
f.rfc8888_rpt_ecn = ProtoField.uint16("udpprague.rfc8888_rpt_ecn", "RFC8888 Report received ECN",    base.DEC,  ipecn_t, 0x6000)
f.rfc8888_rpt_ato = ProtoField.uint16("udpprague.rfc8888_rpt_ato", "RFC8888 Report air time offset", base.DEC,  nil,     0x1FFF)

function udpprague_p.dissector(buffer, pinfo, tree)

	-- Changing the value in the protocol column (the Wireshark pane that displays a list of packets)
	pinfo.cols.protocol = "UDP_PRAGUE"
	
	-- Get the message type first
	local offset = 0
	local length = 1
	local msg_type = buffer(offset, length):uint()
	local payload_len = buffer:len()

	if msg_type == 1 then
		if payload_len >= 13 then
			offset = 0
			length = 13
			local subtree = tree:add(udpprague_p, buffer(offset, length), "UDP Prague Protocol")
			subtree:add(f.type,       buffer(offset, 1)); offset = offset + 1
			subtree:add(f.timestamp,  buffer(offset, 4)); offset = offset + 4
			subtree:add(f.echoed_ts,  buffer(offset, 4)); offset = offset + 4
			subtree:add(f.seq_nr,     buffer(offset, 4)); offset = offset + 4
		else
			offset = 0
			length = 0
			--subtree:add_expert_info(PI_MALFORMED, PI_ERROR, "Invalid bulk sender data length: " .. payload_len .. " bytes")
		end
		-- Handover remaining part to data dissector
		local data_buffer = buffer:range(offset, payload_len - length):tvb()
		Dissector.get("data"):call(data_buffer, pinfo, tree)
	elseif msg_type == 2 then
		if payload_len >= 25 then
			offset = 0
			length = 25
			local subtree = tree:add(udpprague_p, buffer(offset, length), "UDP Prague Protocol")
			subtree:add(f.type,        buffer(offset, 1)); offset = offset + 1
			subtree:add(f.timestamp,   buffer(offset, 4)); offset = offset + 4
			subtree:add(f.echoed_ts,   buffer(offset, 4)); offset = offset + 4
			subtree:add(f.seq_nr,      buffer(offset, 4)); offset = offset + 4
			subtree:add(f.frame_nr,    buffer(offset, 4)); offset = offset + 4
			subtree:add(f.frame_sent,  buffer(offset, 4)); offset = offset + 4
			subtree:add(f.frame_size,  buffer(offset, 4)); offset = offset + 4
		else
			offset = 0
			length = 0
			--subtree:add_expert_info(PI_MALFORMED, PI_ERROR, "Invalid real-time data length: " .. payload_len .. " bytes")
		end
		-- Handover remaining part to data dissector
		local data_buffer = buffer:range(offset, payload_len - length):tvb()
		Dissector.get("data"):call(data_buffer, pinfo, tree)
	elseif msg_type == 17 then
		if payload_len == 26 then
			offset = 0
			length = payload_len
			local subtree = tree:add(udpprague_p, buffer(offset, length), "UDP Prague Protocol")
			subtree:add(f.type,        buffer(offset, 1)); offset = offset + 1
			subtree:add(f.timestamp,   buffer(offset, 4)); offset = offset + 4
			subtree:add(f.echoed_ts,   buffer(offset, 4)); offset = offset + 4
			subtree:add(f.ack_seq,     buffer(offset, 4)); offset = offset + 4
			subtree:add(f.pkt_rcvd,    buffer(offset, 4)); offset = offset + 4
			subtree:add(f.pkt_ce,      buffer(offset, 4)); offset = offset + 4
			subtree:add(f.pkt_lost,    buffer(offset, 4)); offset = offset + 4
			subtree:add(f.error_l4s,   buffer(offset, 1)); offset = offset + 1
		else
			offset = 0
			length = 0
			--subtree:add_expert_info(PI_MALFORMED, PI_ERROR, "Invalid per-packet ACK length: " .. payload_len .. " bytes")

			-- Handover remaining part to data dissector
			local data_buffer = buffer:range(offset, payload_len - length):tvb()
			Dissector.get("data"):call(data_buffer, pinfo, tree)
		end
	elseif msg_type == 18 then
		local num_offset = offset + 5
		local num_length = 2
		local rpt_num = buffer(num_offset, num_length):uint()
		if payload_len == (2 * rpt_num + 7) then
			offset = 0
			length = payload_len

			local subtree = tree:add(udpprague_p, buffer(offset, length), "UDP Prague Protocol")
			subtree:add(f.type,        buffer(offset, 1)); offset = offset + 1
			subtree:add(f.rfc8888_seq, buffer(offset, 4)); offset = offset + 4
			subtree:add(f.rfc8888_num, buffer(offset, 2)); offset = offset + 2
			for i = 1,rpt_num,1 do
				local subsubstree = subtree:add(f.rfc8888_rpt, buffer(offset, 2))
				subsubstree:add(f.rfc8888_rpt_rcv, buffer(offset, 2))
				subsubstree:add(f.rfc8888_rpt_ecn, buffer(offset, 2))
				subsubstree:add(f.rfc8888_rpt_ato, buffer(offset, 2))
				offset = offset + 2
			end
		else
			offset = 0
			length = 0
			--subtree:add_expert_info(PI_MALFORMED, PI_ERROR, "Invalid RFC8888 ACK length: " .. payload_len .. " bytes")

			-- Handover remaining part to data dissector
			local data_buffer = buffer:range(offset, payload_len - length):tvb()
			Dissector.get("data"):call(data_buffer, pinfo, tree)
		end
	end
end

-- We register our protocol on a UDP port
local udp_port = DissectorTable.get("udp.port")
udp_port:add(UDPPRAGUE_PORT, udpprague_p)
