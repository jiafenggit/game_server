/*
 *  Created on: Dec 16, 2015
 *      Author: zhangyalei
 */


#include "Gate_Manager.h"
#include "Gate_Player.h"
#include "Gate_Timer.h"
#include "Log.h"
#include "DB_Manager.h"
#include "Common_Func.h"

Gate_Player::Gate_Player(void) : cid_(0) { }

Gate_Player::~Gate_Player(void) { }

void Gate_Player::reset(void) {
	cid_ = 0;
	msg_info_.reset();
	recycle_tick_.reset();
	player_info_.reset();
}

int Gate_Player::respond_finer_result(int msg_id, Block_Buffer *buf) {
	return respond_error_result(msg_id, 0, buf);
}

int Gate_Player::respond_error_result(int msg_id, int err, Block_Buffer *buf) {
	if (buf == 0) {
		Block_Buffer msg_buf;
		msg_buf.make_message(msg_id, err);
		msg_buf.finish_message();
		return GATE_MANAGER->send_to_client(cid_, msg_buf);
	} else {
		if ((size_t)buf->get_read_idx() < (sizeof(uint16_t) + sizeof(uint32_t) + sizeof(int32_t))) {
			MSG_USER("Block_Buffer space error !");
			return 0;
		}
		/// insert head msg_id, status
		size_t rd_idx = buf->get_read_idx();
		size_t wr_idx = buf->get_write_idx();

		size_t head_len = sizeof(uint16_t) + sizeof(uint32_t) + sizeof(int32_t); ///len(uint16_t), msg_id(uint32_t),  player_cid(int32_t), status(int32_t)

		buf->set_read_idx(buf->get_read_idx() - head_len);
		uint16_t len = buf->readable_bytes() - sizeof(uint16_t);
		buf->set_write_idx(buf->get_read_idx());

		buf->write_uint16(len);
		buf->write_uint32(msg_id);
		buf->write_uint32(err); /// status
		buf->set_write_idx(wr_idx);
		GATE_MANAGER->send_to_client(cid_, *buf);

		buf->set_read_idx(rd_idx); /// 复位传入的buf参数

		return 0;
	}
}

int Gate_Player::send_to_client(Block_Buffer &buf) {
	return GATE_MANAGER->send_to_client(cid_, buf);
}

int Gate_Player::tick(Time_Value &now) {
	if (recycle_tick(now) == 1)
		return 0;

	return 0;
}

int Gate_Player::link_close() {
	if (recycle_tick_.status_ == Recycle_Tick::RECYCLE)
		return 0;

	set_recycle();

	Block_Buffer buf;
	buf.make_message(SYNC_GATE_GAME_PLAYER_SIGNOUT, 0, cid_);
	MSG_113000 msg;
	msg.role_id = player_info_.role_id;
	msg.serialize(buf);
	buf.finish_message();
	GATE_MANAGER->send_to_game(buf);
	return 0;
}

int Gate_Player::verify_msg_info(uint32_t serial_cipher, uint32_t msg_time_cipher) {
	if (! msg_info_.is_inited) {
		msg_info_.hash_key = elf_hash(player_info_.account.c_str(), player_info_.account.length());
	}
	uint32_t serial = serial_cipher ^ msg_info_.hash_key;
	uint32_t msg_time = msg_time_cipher ^ serial_cipher;

	if (! msg_info_.is_inited) {
		msg_info_.is_inited = true;
		msg_info_.msg_serial = serial; /// 第一条消息序号
		msg_info_.msg_timestamp = GATE_MANAGER->tick_time(); /// 第一条消息时间戳
		msg_info_.msg_interval_count_ = 1;
		msg_info_.msg_interval_timestamp = GATE_MANAGER->tick_time();
		return 0;
	}

	/// 操作频率统计
	if (GATE_MANAGER->tick_time() - msg_info_.msg_interval_timestamp < Time_Value(3, 0)) {
		if (msg_info_.msg_interval_count_ > 90) {
			return -1;
		}
	} else {
		msg_info_.msg_interval_count_ = 0;
		msg_info_.msg_interval_timestamp = GATE_MANAGER->tick_time();
	}

	/// 判断包序号
	if (serial <= msg_info_.msg_serial || serial - msg_info_.msg_serial > static_cast<uint32_t>(10)) {
		MSG_USER("serial = %d, msg_detail_.msg_serial = %d", serial, msg_info_.msg_serial);
		return -2;
	}

	/// 判断包时间戳
	Time_Value msg_time_tv(msg_time, 0);
	if (std::abs(msg_time - msg_info_.msg_timestamp.sec()) > 900) {
		MSG_USER("msg_time = %d, msg_timestamp = %d", msg_time, msg_info_.msg_timestamp.sec());
		return -3;
	}

	msg_info_.msg_serial = serial;
	msg_info_.msg_timestamp = msg_time_tv;
	++msg_info_.msg_interval_count_;

	return 0;
}

int Gate_Player::recycle_tick(const Time_Value &now) {
	int ret = 0;
	if (now - recycle_tick_.last_tick_ts_ > Recycle_Tick::tick_interval_) {
		recycle_tick_.last_tick_ts_ = now;
		if (recycle_tick_.status_ == Recycle_Tick::RECYCLE && now - recycle_tick_.last_change_status_ts_ > Recycle_Tick::recycle_time_) {
			ret = 1;
			GATE_MANAGER->unbind_gate_player(*this);
			reset();
			GATE_MANAGER->push_gate_player(this);
		}
	}
	return ret;
}