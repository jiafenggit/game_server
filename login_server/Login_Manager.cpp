/*
 *  Created on: Dec 16, 2015
 *      Author: zhangyalei
 */


#include "Common_Func.h"
#include "Configurator.h"
#include "Log.h"
#include "Login_Server.h"
#include "Login_Client_Messager.h"
#include "Login_Inner_Messager.h"
#include "Login_Manager.h"

Login_Manager::Login_Manager(void):
  is_register_timer_(false),
  msg_count_onoff_(true),
  accout_session_map_(get_hash_table_size(12000)) {
	register_timer();
}

Login_Manager::~Login_Manager(void) {
	unregister_timer();
	check_account_.release_mysql_conn();
}

Login_Manager *Login_Manager::instance_;

Login_Manager *Login_Manager::instance(void) {
	if (instance_ == 0)
		instance_ = new Login_Manager;
	return instance_;
}

int Login_Manager::init(void) {
	tick_time_ = Time_Value::gettimeofday();
	status_ = STATUS_NORMAL;

	CONFIG_INSTANCE;
	LOGIN_INNER_MESSAGER;					/// 内部消息处理
	LOGIN_CLIENT_MESSAGER;					/// 外部消息处理
	LOGIN_TIMER->thr_create();
	check_account_.connect_mysql_db();
	init_gate_ip();

	return 0;
}

void Login_Manager::run_handler(void) {
	process_list();
}

int Login_Manager::init_gate_ip(void) {
	const Json::Value &server_maintainer = CONFIG_INSTANCE->server_maintainer();
	if (server_maintainer == Json::Value::null) {
		MSG_ABORT("configure file error.");
		return -1;
	}

	Ip_Info ip_info;
	for (Json::Value::iterator iter = server_maintainer["gate_server_list"].begin();
			iter != server_maintainer["gate_server_list"].end(); ++iter) {
		ip_info.ip = (*iter)["ip"].asString();
		ip_info.port = (*iter)["port"].asInt();
	}
	gate_ip_vec_.push_back(ip_info);

	return 0;
}

void Login_Manager::get_gate_ip(std::string &account, std::string &ip, int &port) {
	long hash = elf_hash(account.c_str(), account.size());
	int index = hash % (gate_ip_vec_.size());
	ip = gate_ip_vec_[index].ip;
	port = gate_ip_vec_[index].port;
}

int Login_Manager::bind_account_session(std::string& account, std::string& session) {
	if (! accout_session_map_.insert(std::make_pair(account, session)).second) {
		MSG_USER("insert failure");
	}
	return 0;
}

int Login_Manager::find_account_session(std::string& account, std::string& session){
	Account_Session_Map::iterator iter = accout_session_map_.find(account);
	if (iter != accout_session_map_.end() && iter->second == session){
		return 0;
	} else {
		return -1;
	}
}

int Login_Manager::unbind_account_session(std::string& account){
	accout_session_map_.erase(account);
	return 0;
}

int Login_Manager::register_timer(void) {
	is_register_timer_ = true;
	return 0;
}

int Login_Manager::unregister_timer(void) {
	is_register_timer_ = false;
	return 0;
}

int Login_Manager::send_to_client(int cid, Block_Buffer &buf) {
	if (cid < 2) {
		MSG_USER("cid = %d", cid);
		return -1;
	}
	return LOGIN_CLIENT_SERVER->send_block(cid, buf);
}

int Login_Manager::send_to_gate(int cid, Block_Buffer &buf){
	if (cid < 2) {
			MSG_USER("cid = %d", cid);
			return -1;
	}

	return LOGIN_GATE_SERVER->send_block(cid, buf);
}

int Login_Manager::close_client(int cid) {
	if (cid >= 2) {
		Close_Info info(cid, tick_time());
		close_list_.push_back(info);
	} else {
		MSG_USER_TRACE("cid < 2");
	}
	return 0;
}

int Login_Manager::process_list(void) {
	int32_t cid = 0;
	Block_Buffer *buf = 0;

	while (1) {
		bool all_empty = true;

		/// client-->login
		if ((buf = login_client_data_list_.pop_front()) != 0) {
			all_empty = false;
			if (buf->is_legal()) {
				buf->peek_int32(cid);
				LOGIN_CLIENT_MESSAGER->process_block(*buf);
			} else {
				MSG_USER("buf.read_index = %ld, buf.write_index = %ld",
						buf->get_read_idx(), buf->get_write_idx());
				buf->reset();
			}
			LOGIN_CLIENT_SERVER->push_block(cid, buf);
		}
		/// gate-->login
		if ((buf = login_gate_data_list_.pop_front()) != 0) {
			all_empty = false;
			if (buf->is_legal()) {
				buf->peek_int32(cid);
				LOGIN_INNER_MESSAGER->process_gate_block(*buf);
			} else {
				MSG_USER("buf.read_index = %ld, buf.write_index = %ld",
						buf->get_read_idx(), buf->get_write_idx());
				buf->reset();
			}
			LOGIN_GATE_SERVER->push_block(cid, buf);
		}
		/// 游戏服内部循环消息队列
		if (! self_loop_block_list_.empty()) {
			all_empty = false;
			buf = self_loop_block_list_.front();
			self_loop_block_list_.pop_front();
			LOGIN_INNER_MESSAGER->process_self_loop_block(*buf);
			block_pool_.push(buf);
		}

		if (all_empty)
			Time_Value::sleep(SLEEP_TIME);
	}
	return 0;
}

int Login_Manager::server_status(void) {
	return status_;
}

int Login_Manager::self_close_process(void) {
	status_ = STATUS_CLOSING;
	return 0;
}

int Login_Manager::tick(void) {
	Time_Value now(Time_Value::gettimeofday());
	tick_time_ = now;

	close_list_tick(now);
	manager_tick(now);

	server_info_tick(now);
	object_pool_tick(now);
	//LOG->show_msg_time(now);
	return 0;
}

int Login_Manager::close_list_tick(Time_Value &now) {
	Close_Info info;
	while (! close_list_.empty()) {
		info = close_list_.front();
		if (now - info.timestamp > Time_Value(2, 0)) {
			close_list_.pop_front();
			LOGIN_CLIENT_SERVER->receive().push_drop(info.cid);
		} else {
			break;
		}
	}
	return 0;
}

int Login_Manager::server_info_tick(Time_Value &now) {
	if (now - tick_info_.server_info_last_tick < tick_info_.server_info_interval_tick)
		return 0;

	tick_info_.server_info_last_tick = now;

	login_gate_server_info_.reset();
	login_client_server_info_.reset();
	LOGIN_GATE_SERVER->get_server_info(login_gate_server_info_);
	LOGIN_CLIENT_SERVER->get_server_info(login_client_server_info_);

	return 0;
}

int Login_Manager::manager_tick(Time_Value &now) {
	if (now - tick_info_.manager_last_tick < tick_info_.manager_interval_tick)
		return 0;
	tick_info_.manager_last_tick = now;
	return 0;
}

void Login_Manager::object_pool_tick(Time_Value &now) {
	if (now - tick_info_.object_pool_last_tick < tick_info_.object_pool_interval_tick)
		return;
	tick_info_.object_pool_last_tick = now;
	object_pool_size();
}

void Login_Manager::get_server_info(Block_Buffer &buf) {
	login_client_server_info_.serialize(buf);
	login_gate_server_info_.serialize(buf);
}

void Login_Manager::object_pool_size(void) {
	MSG_DEBUG("Login_Mangager Object_Pool Size ==============================================================");
	MSG_DEBUG("block_pool_ free = %d, used = %d", block_pool_.free_obj_list_size(), block_pool_.used_obj_list_size());
}

void Login_Manager::free_cache(void) {
	MSG_DEBUG("REQ_FREE_CACHE");

	LOGIN_CLIENT_SERVER->free_cache();
	LOGIN_GATE_SERVER->free_cache();

	block_pool_.shrink_all();
}

void Login_Manager::print_msg_count(void) {
	std::stringstream stream;
	for (Msg_Count_Map::iterator it = inner_msg_count_map_.begin(); it != inner_msg_count_map_.end(); ++it) {
		stream << (it->first) << "\t" << (it->second) << std::endl;
	}
	MSG_USER("inner_msg_count_map_.size = %d\n%s\n", inner_msg_count_map_.size(), stream.str().c_str());
}
