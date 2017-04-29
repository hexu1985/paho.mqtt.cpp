// async_client.cpp

/*******************************************************************************
 * Copyright (c) 2013-2016 Frank Pagliughi <fpagliughi@mindspring.com>
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 *
 * The Eclipse Public License is available at
 *    http://www.eclipse.org/legal/epl-v10.html
 * and the Eclipse Distribution License is available at
 *   http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * Contributors:
 *    Frank Pagliughi - initial implementation and documentation
 *******************************************************************************/

#include "mqtt/async_client.h"
#include "mqtt/token.h"
#include "mqtt/message.h"
#include "mqtt/response_options.h"
#include "mqtt/disconnect_options.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstring>
#include <cstdio>

namespace mqtt {

/////////////////////////////////////////////////////////////////////////////

async_client::async_client(const string& serverURI, const string& clientId,
						   const string& persistDir)
				: async_client(serverURI, clientId, 0, persistDir)
{
}

async_client::async_client(const string& serverURI, const string& clientId,
						   iclient_persistence* persistence /*=nullptr*/)
				: async_client(serverURI, clientId, 0, persistence)
{
}

async_client::async_client(const string& serverURI, const string& clientId,
						   int maxBufferedMessages, const string& persistDir)
				: serverURI_(serverURI), clientId_(clientId),
					persist_(nullptr), userCallback_(nullptr)
{
	std::unique_ptr<MQTTAsync_createOptions> opts;
	if (maxBufferedMessages != 0) {
		opts.reset(new MQTTAsync_createOptions MQTTAsync_createOptions_initializer);
		opts->sendWhileDisconnected = !0;
		opts->maxBufferedMessages = maxBufferedMessages;
	}

	MQTTAsync_createWithOptions(&cli_, serverURI.c_str(), clientId.c_str(),
								MQTTCLIENT_PERSISTENCE_DEFAULT,
								const_cast<char*>(persistDir.c_str()),
								opts.get());
}

async_client::async_client(const string& serverURI, const string& clientId,
						   int maxBufferedMessages, iclient_persistence* persistence /*=nullptr*/)
				: serverURI_(serverURI), clientId_(clientId),
					persist_(nullptr), userCallback_(nullptr)
{
	std::unique_ptr<MQTTAsync_createOptions> opts;
	if (maxBufferedMessages != 0) {
		opts.reset(new MQTTAsync_createOptions MQTTAsync_createOptions_initializer);
		opts->sendWhileDisconnected = !0;
		opts->maxBufferedMessages = maxBufferedMessages;
	}

	if (!persistence) {
		MQTTAsync_createWithOptions(&cli_, serverURI.c_str(), clientId.c_str(),
									MQTTCLIENT_PERSISTENCE_NONE, nullptr, opts.get());
	}
	else {
		persist_.reset(new MQTTClient_persistence {
			persistence,
			&iclient_persistence::persistence_open,
			&iclient_persistence::persistence_close,
			&iclient_persistence::persistence_put,
			&iclient_persistence::persistence_get,
			&iclient_persistence::persistence_remove,
			&iclient_persistence::persistence_keys,
			&iclient_persistence::persistence_clear,
			&iclient_persistence::persistence_containskey
		});

		MQTTAsync_createWithOptions(&cli_, serverURI.c_str(), clientId.c_str(),
									MQTTCLIENT_PERSISTENCE_USER, persist_.get(),
									opts.get());
	}
}

async_client::~async_client()
{
	MQTTAsync_destroy(&cli_);
}

// --------------------------------------------------------------------------
// Class static callbacks.
// These are the callbacks directly from the C-lib. In each case the
// 'context' should be the address of the async_client object that
// registered the callback.

void async_client::on_connection_lost(void *context, char *cause)
{
	if (context) {
		async_client* cli = static_cast<async_client*>(context);
		callback* cb = cli->userCallback_;

		if (cb)
			cb->connection_lost(cause ? string(cause) : string());
	}
}

int async_client::on_message_arrived(void* context, char* topicName, int topicLen,
									 MQTTAsync_message* msg)
{
	if (context) {
		async_client* cli = static_cast<async_client*>(context);
		callback* cb = cli->userCallback_;
		consumer_queue_type& que = cli->que_;

		if (cb || que) {
			string topic(topicName, topicName+topicLen);
			auto m = std::make_shared<message>(*msg);

			if (cb)
				cb->message_arrived(topic, m);
			if (que)
				que->put(std::make_tuple(topic, m));
		}
	}

	MQTTAsync_freeMessage(&msg);
	MQTTAsync_free(topicName);

	// TODO: Should the user code determine the return value?
	// The Java version does doesn't seem to...
	return (!0);
}

// Callback to indicate that a message was delivered to the server.
// It is called for a message with a QOS >= 1, but it happens before the
// on_success() call for the token. Thus we don't have the underlying
// MQTTAsync_token of the outgoing message at the time of this callback.
//
// *** So using the Async C library we have no way to match this msgID with
//     a delivery_token object. So this is useless to us.
//
// So, all in all, this callback in it's current implementation seems rather
// redundant.
//
#if 0
void async_client::on_delivery_complete(void* context, MQTTAsync_token msgID)
{
	if (context) {
		async_client* m = static_cast<async_client*>(context);
		callback* cb = m->get_callback();
		if (cb) {
			delivery_token_ptr tok = m->get_pending_delivery_token(msgID);
			cb->delivery_complete(tok);
		}
	}
}
#endif

// --------------------------------------------------------------------------
// Private methods

void async_client::add_token(token_ptr tok)
{
	if (tok) {
		guard g(lock_);
		pendingTokens_.push_back(tok);
	}
}

void async_client::add_token(delivery_token_ptr tok)
{
	if (tok) {
	   guard g(lock_);
	   pendingDeliveryTokens_.push_back(tok);
	}
}

// Note that we uniquely identify a token by the address of its raw pointer,
// since the message ID is not unique.

void async_client::remove_token(token* tok)
{
	if (!tok)
		return;

	guard g(lock_);
	for (auto p=pendingDeliveryTokens_.begin();
					p!=pendingDeliveryTokens_.end(); ++p) {
		if (p->get() == tok) {
			delivery_token_ptr dtok = *p;
			pendingDeliveryTokens_.erase(p);

			// If there's a user callback registered, we can now call
			// delivery_complete()

			if (userCallback_) {
				const_message_ptr msg = dtok->get_message();
				if (msg && msg->get_qos() > 0) {
					callback* cb = userCallback_;
					g.unlock();
					cb->delivery_complete(dtok);
				}
			}
			return;
		}
	}
	for (auto p=pendingTokens_.begin(); p!=pendingTokens_.end(); ++p) {
		if (p->get() == tok) {
			pendingTokens_.erase(p);
			return;
		}
	}
}

// --------------------------------------------------------------------------
// Connect

token_ptr async_client::connect()
{ 
	return connect(connect_options());
}

token_ptr async_client::connect(connect_options opts)
{
	auto tok = token::create(*this);
	add_token(tok);

	opts.set_token(tok);

	int rc = MQTTAsync_connect(cli_, &opts.opts_);

	if (rc != MQTTASYNC_SUCCESS) {
		remove_token(tok);
		throw exception(rc);
	}

	return tok;
}

token_ptr async_client::connect(connect_options opts, void* userContext,
								iaction_listener& cb)
{
	auto tok = token::create(*this, userContext, cb);
	add_token(tok);

	opts.set_token(tok);

	int rc = MQTTAsync_connect(cli_, &opts.opts_);

	if (rc != MQTTASYNC_SUCCESS) {
		remove_token(tok);
		throw exception(rc);
	}

	return tok;
}

token_ptr async_client::connect(void* userContext, iaction_listener& cb)
{
	connect_options opts;
	opts.opts_.keepAliveInterval = 30;
	opts.opts_.cleansession = 1;
	return connect(opts, userContext, cb);
}

// --------------------------------------------------------------------------
// Re-connect

token_ptr async_client::reconnect()
{
	auto tok = token::create(*this);
	add_token(tok);

	int rc = MQTTAsync_setConnected(cli_, tok.get(), &token::on_connected);

	if (rc == MQTTASYNC_SUCCESS)
		rc = MQTTAsync_reconnect(cli_);

	if (rc != MQTTASYNC_SUCCESS) {
		MQTTAsync_setConnected(cli_, nullptr, nullptr);
		remove_token(tok);
		throw exception(rc);
	}

	return tok;
}

// --------------------------------------------------------------------------
// Disconnect

token_ptr async_client::disconnect(disconnect_options opts)
{
	auto tok = token::create(*this);
	add_token(tok);

	opts.set_token(tok);

	int rc = MQTTAsync_disconnect(cli_, &opts.opts_);

	if (rc != MQTTASYNC_SUCCESS) {
		remove_token(tok);
		throw exception(rc);
	}

	return tok;
}

token_ptr async_client::disconnect(int timeout)
{
	auto tok = token::create(*this);
	add_token(tok);

	disconnect_options opts(timeout, tok);

	int rc = MQTTAsync_disconnect(cli_, &opts.opts_);

	if (rc != MQTTASYNC_SUCCESS) {
		remove_token(tok);
		throw exception(rc);
	}

	return tok;
}

token_ptr async_client::disconnect(int timeout, void* userContext, iaction_listener& cb)
{
	auto tok = token::create(*this, userContext, cb);
	add_token(tok);

	disconnect_options opts(timeout, tok);

	int rc = MQTTAsync_disconnect(cli_, &opts.opts_);

	if (rc != MQTTASYNC_SUCCESS) {
		remove_token(tok);
		throw exception(rc);
	}

	return tok;
}

// --------------------------------------------------------------------------
// Queries

delivery_token_ptr async_client::get_pending_delivery_token(int msgID) const
{
	// Messages with QOS=1 or QOS=2 that require a response/acknowledge should
	// have a non-zero 16-bit message ID. The library keeps the token objects
	// for all of these messages that are in flight. When the acknowledge comes
	// back from the broker, the C++ library can look up the token from the
	// msgID and signal it, indicating completion.

	if (msgID > 0) {
		guard g(lock_);
		for (const auto& t : pendingDeliveryTokens_) {
			if (t->get_message_id() == msgID)
				return t;
		}
	}
	return delivery_token_ptr();
}

std::vector<delivery_token_ptr> async_client::get_pending_delivery_tokens() const
{
	std::vector<delivery_token_ptr> toks;
	guard g(lock_);
	for (const auto& t : pendingDeliveryTokens_) {
		if (t->get_message_id() > 0) {
			toks.push_back(t);
		}
	}
	return toks;
}

// --------------------------------------------------------------------------
// Publish

delivery_token_ptr async_client::publish(const string& topic, const void* payload,
										 size_t n, int qos, bool retained)
{
	auto msg = make_message(payload, n, qos, retained);
	return publish(topic, std::move(msg));
}

delivery_token_ptr async_client::publish(const string& topic, binary_ref payload,
										 int qos, bool retained)
{
	auto msg = make_message(payload, qos, retained);
	return publish(topic, std::move(msg));
}

delivery_token_ptr async_client::publish(const string& topic,
										 const void* payload, size_t n,
										 int qos, bool retained, void* userContext,
										 iaction_listener& cb)
{
	auto msg = make_message(payload, n, qos, retained);
	return publish(topic, std::move(msg), userContext, cb);
}

delivery_token_ptr async_client::publish(const string& topic, const_message_ptr msg)
{
	auto tok = delivery_token::create(*this, topic, msg);
	add_token(tok);

	delivery_response_options opts(tok);

	int rc = MQTTAsync_sendMessage(cli_, topic.c_str(), &(msg->msg_), &opts.opts_);

	if (rc == MQTTASYNC_SUCCESS) {
		tok->set_message_id(opts.opts_.token);
	}
	else {
		remove_token(tok);
		throw exception(rc);
	}

	return tok;
}

delivery_token_ptr async_client::publish(const string& topic, const_message_ptr msg,
										 void* userContext, iaction_listener& cb)
{
	delivery_token_ptr tok = delivery_token::create(*this, topic, msg, userContext, cb);
	add_token(tok);

	delivery_response_options opts(tok);

	int rc = MQTTAsync_sendMessage(cli_, topic.c_str(), &(msg->msg_), &opts.opts_);

	if (rc == MQTTASYNC_SUCCESS) {
		tok->set_message_id(opts.opts_.token);
	}
	else {
		remove_token(tok);
		throw exception(rc);
	}

	return tok;
}

// --------------------------------------------------------------------------

void async_client::set_callback(callback& cb)
{
	guard g(lock_);
	userCallback_ = &cb;

	int rc = MQTTAsync_setCallbacks(cli_, this,
									&async_client::on_connection_lost,
									&async_client::on_message_arrived,
									nullptr /*&async_client::on_delivery_complete*/);

	if (rc != MQTTASYNC_SUCCESS)
		throw exception(rc);
}

// --------------------------------------------------------------------------
// Subscribe

token_ptr async_client::subscribe(const_string_collection_ptr topicFilters,
								   const qos_collection& qos)

{
	size_t n = topicFilters->size();

	if (n != qos.size())
		throw std::invalid_argument("Collection sizes don't match");

	auto tok = token::create(*this, topicFilters);
	add_token(tok);

	response_options opts(tok);

	int rc = MQTTAsync_subscribeMany(cli_, int(n), topicFilters->c_arr(),
									 const_cast<int*>(&qos[0]), &opts.opts_);

	if (rc != MQTTASYNC_SUCCESS) {
		remove_token(tok);
		throw exception(rc);
	}

	return tok;
}

token_ptr async_client::subscribe(const_string_collection_ptr topicFilters,
								  const qos_collection& qos,
								  void* userContext, iaction_listener& cb)
{
	size_t n = topicFilters->size();

	if (n != qos.size())
		throw std::invalid_argument("Collection sizes don't match");

	auto tok = token::create(*this, topicFilters, userContext, cb);
	add_token(tok);

	response_options opts(tok);

	int rc = MQTTAsync_subscribeMany(cli_, int(n), topicFilters->c_arr(),
									 const_cast<int*>(&qos[0]), &opts.opts_);

	if (rc != MQTTASYNC_SUCCESS) {
		remove_token(tok);
		throw exception(rc);
	}

	return tok;
}

token_ptr async_client::subscribe(const string& topicFilter, int qos)
{
	auto tok = token::create(*this, topicFilter);
	add_token(tok);

	response_options opts(tok);

	int rc = MQTTAsync_subscribe(cli_, topicFilter.c_str(), qos, &opts.opts_);

	if (rc != MQTTASYNC_SUCCESS) {
		remove_token(tok);
		throw exception(rc);
	}

	return tok;
}

token_ptr async_client::subscribe(const string& topicFilter, int qos,
								   void* userContext, iaction_listener& cb)
{
	auto tok = token::create(*this, topicFilter, userContext, cb);
	add_token(tok);

	response_options opts(tok);

	int rc = MQTTAsync_subscribe(cli_, topicFilter.c_str(), qos, &opts.opts_);

	if (rc != MQTTASYNC_SUCCESS) {
		remove_token(tok);
		throw exception(rc);
	}

	return tok;
}

// --------------------------------------------------------------------------
// Unsubscribe

token_ptr async_client::unsubscribe(const string& topicFilter)
{
	auto tok = token::create(*this, topicFilter);
	add_token(tok);

	response_options opts(tok);

	int rc = MQTTAsync_unsubscribe(cli_, topicFilter.c_str(), &opts.opts_);

	if (rc != MQTTASYNC_SUCCESS) {
		remove_token(tok);
		throw exception(rc);
	}

	return tok;
}

token_ptr async_client::unsubscribe(const_string_collection_ptr topicFilters)
{
	size_t n = topicFilters->size();

	auto tok = token::create(*this, topicFilters);
	add_token(tok);

	response_options opts(tok);

	int rc = MQTTAsync_unsubscribeMany(cli_, int(n),
									   topicFilters->c_arr(), &opts.opts_);

	if (rc != MQTTASYNC_SUCCESS) {
		remove_token(tok);
		throw exception(rc);
	}

	return tok;
}

token_ptr async_client::unsubscribe(const_string_collection_ptr topicFilters,
									 void* userContext, iaction_listener& cb)
{
	size_t n = topicFilters->size();

	auto tok = token::create(*this, topicFilters, userContext, cb);
	add_token(tok);

	response_options opts(tok);

	int rc = MQTTAsync_unsubscribeMany(cli_, int(n), topicFilters->c_arr(), &opts.opts_);

	if (rc != MQTTASYNC_SUCCESS) {
		remove_token(tok);
		throw exception(rc);
	}

	return tok;
}

token_ptr async_client::unsubscribe(const string& topicFilter,
									void* userContext, iaction_listener& cb)
{
	auto tok = token::create(*this, topicFilter, userContext, cb);
	add_token(tok);

	response_options opts(tok);

	int rc = MQTTAsync_unsubscribe(cli_, topicFilter.c_str(), &opts.opts_);

	if (rc != MQTTASYNC_SUCCESS) {
		remove_token(tok);
		throw exception(rc);
	}

	return tok;
}

// --------------------------------------------------------------------------

void async_client::start_consuming()
{
	// TODO: Should we replace user callback?
	//userCallback_ = nullptr;

	// TODO: Check if callbacks already set/running, otherwise we have race
	// conditions and/or stomping other callbacks
	que_.reset(new thread_queue<consumer_message_type>);

	int rc = MQTTAsync_setCallbacks(cli_, this,
									&async_client::on_connection_lost,
									&async_client::on_message_arrived,
									nullptr);

	if (rc != MQTTASYNC_SUCCESS)
		throw exception(rc);
}

void async_client::stop_consuming()
{
	int rc = MQTTAsync_setCallbacks(cli_, this, nullptr, nullptr, nullptr);
	que_.reset(new thread_queue<consumer_message_type>);

	if (rc != MQTTASYNC_SUCCESS)
		throw exception(rc);
}

/////////////////////////////////////////////////////////////////////////////
// end namespace mqtt
}

