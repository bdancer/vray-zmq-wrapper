#ifndef _ZMQ_WRAPPER_H_
#define _ZMQ_WRAPPER_H_

#define NOMINMAX // zmq includes windows.h
#include <zmq.hpp>

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <memory>
#include <deque>
#include <mutex>
#include <cstdio>

#include <chrono>
#include <condition_variable>
#include <random>
#include <limits>

#include "base_types.h"
#include "zmq_message.hpp"

static const int ZMQ_PROTOCOL_VERSION = 1013;

static const int CLIENT_PING_INTERVAL = 1000;
static const int SOCKET_IO_TIMEOUT = 100;

#ifdef _DEBUG
static const int EXPORTER_TIMEOUT = 1 << 29;
static const int HEARBEAT_TIMEOUT = 1 << 29;
#else
static const int EXPORTER_TIMEOUT = CLIENT_PING_INTERVAL * 5;
static const int HEARBEAT_TIMEOUT = CLIENT_PING_INTERVAL * 2;
#endif

static const int MAX_CONSEQ_MESSAGES = 10;

enum class ClientType: int {
	None,
	Exporter,
	Heartbeat,
};

enum class ControlMessage: int {
	DATA_MSG = 0,

	EXPORTER_CONNECT_MSG = 1000,
	HEARTBEAT_CONNECT_MSG = 1001,

	RENDERER_CREATE_MSG = 2000,
	HEARTBEAT_CREATE_MSG = 2001,

	PING_MSG = 3000,
	PONG_MSG = 3001,

	STOP_MSG = 4000,
};


struct ControlFrame {
	int version;
	ClientType type;
	ControlMessage control;

	ControlFrame(ClientType type = ClientType::Exporter, ControlMessage ctrl = ControlMessage::DATA_MSG)
		: version(ZMQ_PROTOCOL_VERSION)
		, type(type)
		, control(ctrl) {}

	explicit ControlFrame(const zmq::message_t & msg) {
		if (msg.size() != sizeof(*this)) {
			version = -1;
		} else {
			memcpy(this, msg.data(), sizeof(*this));
		}
	}

	explicit operator bool() {
		return version == ZMQ_PROTOCOL_VERSION;
	}

	static zmq::message_t make(ClientType type = ClientType::Exporter, ControlMessage ctrl = ControlMessage::DATA_MSG) {
		zmq::message_t msg(sizeof(ControlFrame));
		ControlFrame frame(type, ctrl);
		memcpy(msg.data(), &frame, msg.size());
		return msg;
	}
};


/// Async wrapper for zmq::socket_t with callback on data received.
/// Supports heartbeat mode which will create heartbeat connection with the server that will not be auto-terminated when
/// there is no communication on it from the server side. Used to keep the server alive all the time
/// Objects of this type will start a thread that will enable async send and recieve of data
class ZmqClient {
public:
	typedef std::function<void(const VRayMessage &, ZmqClient *)> ZmqOnMessageCallback;

	/// Create a new client - in unconnected state, call ::connect to initiate connection
	/// @param isHeartbeat create the client in heartbeat mode
	ZmqClient(bool isHeartbeat = false);
	~ZmqClient();

	ZmqClient(const ZmqClient &) = delete;
	ZmqClient &operator=(const ZmqClient &) = delete;

	/// Send data with size, the data will be copied inside and can be safely freed after the function returns
	/// @data - pointer to bytes
	/// @size - number of bytes in data
	void send(const void *data, int size);

	/// Send message while also stealing it's content
	/// @message - the message to send, after the function returns, callee's message is empty
	void send(zmq::message_t && message);

	/// Set a callback to be called on message received (messages discarded if not set)
	void setCallback(ZmqOnMessageCallback cb);

	/// Set or clear flag to flush outstanding messages on stop/exit
	void setFlushOnExit(bool flag);
	/// Check the flush on exit flag
	bool getFlushOnexit() const;

	/// Get number of messages that are yet to be sent to server
	int getOutstandingMessages() const;

	/// Check if the worker is serving
	bool good() const;

	/// Check if currently the socket is connected
	bool connected() const;

	/// Connect to address
	/// @addr - the address to connect to
	void connect(const char * addr);

	/// Send 'stop' command to server as soon as possible
	void stopServer();

	/// Stop the server waiting for the worker thread to join
	void syncStop();

	/// Block until all messages are sent or timeout has passed, if there are no messages, return immediately
	/// @timeout - timeout in milliseconds to wait max
	/// @return - false if there are still messages in queue after wait is finished
	bool waitForMessages(int timeout = 500);

private:

	typedef std::chrono::high_resolution_clock::time_point time_point;

	/// Start function for the worker thread (sends and receives messages)
	void workerThread(volatile bool & socketInit, std::mutex & mtx, std::condition_variable & workerReady);
	/// Send any outstanding messages
	bool workerSendoutMessages(time_point & lastHBSend);

	const ClientType clientType; ///< The type of this client (heartbeat or exporter)
	ZmqOnMessageCallback callback; ///< Callback to be called on received message
	std::mutex callbackMutex; ///< Mutex protecting @callback

	std::thread worker; ///< Thread serving messages and calling the callback

	zmq::context_t context; ///< The zmq context
	std::deque<zmq::message_t> messageQue; ///< Queue with outstanding messages
	std::mutex messageMutex; ///< Mutex protecting @messageQue

	std::condition_variable startServingCond; ///< Cond var to signal the worker thread to start serving
	std::mutex startServingMutex; ///< Mutex protecting @startServing flag
	time_point lastHeartbeat; ///< Last time hartbeat was sent/received

	std::atomic<bool> startServing; ///< Used to signal worker, the socket is connected and serving can start
	std::atomic<bool> isWorking; ///< Flag set to true if the thread is serving requests
	std::atomic<bool> errorConnect; ///< Flag set to true if we could not connect
	std::atomic<bool> flushOnExit; ///< If true when worker is stopping for any reason, outstanding messages will be sent
	std::atomic<bool> serverStop; ///< If true will stop transmitting messages and send 'stop' command to server

	std::unique_ptr<zmq::socket_t> frontend; ///< The zmq socket
};


inline ZmqClient::ZmqClient(bool isHeartbeat)
    : clientType(isHeartbeat ? ClientType::Heartbeat : ClientType::Exporter)
    , context(1)
    , startServing(false)
    , isWorking(true)
    , errorConnect(false)
    , flushOnExit(false)
    , serverStop(false)
    , frontend(nullptr)
{

	bool socketInit = false;
	std::condition_variable threadReady;
	std::mutex threadMutex;

	worker = std::thread(&ZmqClient::workerThread, this, std::ref(socketInit), std::ref(threadMutex), std::ref(threadReady));

	{
		std::unique_lock<std::mutex> lock(threadMutex);
		// wait for the thread to finish initing the socket, else bind & connect might be called before init
		threadReady.wait(lock, [&socketInit] { return socketInit; });
	}
}

inline void ZmqClient::workerThread(volatile bool & socketInit, std::mutex & mtx, std::condition_variable & workerReady) {
	try {

		this->frontend = std::unique_ptr<zmq::socket_t>(new zmq::socket_t(context, ZMQ_DEALER));
		int linger = 0;
		this->frontend->setsockopt(ZMQ_LINGER, &linger, sizeof(linger));

		int wait = HEARBEAT_TIMEOUT;
		this->frontend->setsockopt(ZMQ_SNDTIMEO, &wait, sizeof(wait));

		std::lock_guard<std::mutex> lock(mtx);
		socketInit = true;
	} catch (zmq::error_t & e) {
		printf("ZMQ exception while worker initialization: %s\n", e.what());
		this->isWorking = false;
		workerReady.notify_all();
		return;
	}
	workerReady.notify_all();

	if (!this->startServing) {
		std::unique_lock<std::mutex> lock(this->startServingMutex);
		if (!this->startServing) {
			this->startServingCond.wait(lock, [this]() -> bool { return this->startServing; });
		}
	}

	std::shared_ptr<void> atScopeExit(nullptr, [this] (void *) {
		this->frontend->close();
		this->isWorking = false;
	});

	if (this->errorConnect) {
		return;
	}

	zmq::message_t emptyFrame(0);

	// send handshake
	try {
		if (clientType == ClientType::Exporter) {
			frontend->send(ControlFrame::make(clientType, ControlMessage::EXPORTER_CONNECT_MSG), ZMQ_SNDMORE);
		} else {
			frontend->send(ControlFrame::make(clientType, ControlMessage::HEARTBEAT_CONNECT_MSG), ZMQ_SNDMORE);
		}
		this->frontend->send(emptyFrame);
	} catch (zmq::error_t & ex) {
		printf("ZMQ failed to send handshake [%s]\n", ex.what());
		return;
	}


	// recv handshake
	try {
		int wait = EXPORTER_TIMEOUT;
		this->frontend->setsockopt(ZMQ_RCVTIMEO, &wait, sizeof(wait));

		zmq::message_t controlMsg, emptyMsg;
		bool recv = frontend->recv(&controlMsg);
		if (!recv) {
			puts("ZMQ server did not respond in expected timeout, stopping client!");
			return;
		}
		frontend->recv(&emptyMsg);

		ControlFrame frame(controlMsg);

		if (!frame) {
			printf("ZMQ expected protocol version [%d], server speaks [%d]\n", ZMQ_PROTOCOL_VERSION, frame.version);
			return;
		}

		if (frame.type != clientType) {
			puts("ZMQ server created mismatching type of worker for us!");
			return;
		}

		if (clientType == ClientType::Exporter) {
			if (frame.control != ControlMessage::RENDERER_CREATE_MSG) {
				puts("ZMQ server responded with different than renderer created!");
				return;
			}
		} else {
			if (frame.control != ControlMessage::HEARTBEAT_CREATE_MSG) {
				puts("ZMQ server responded with different than heartbeat created!");
				return;
			}
		}
	} catch (zmq::error_t & ex) {
		printf("ZMQ failed to receive handshake [%s]\n", ex.what());
		return;
	}

	puts("ZMQ connected to server.");

	auto lastHBRecv = std::chrono::high_resolution_clock::now();
	// ensure we send one HB immediately
	auto lastHBSend = lastHBRecv - std::chrono::milliseconds(HEARBEAT_TIMEOUT * 2);

	zmq::pollitem_t pollContext = {*this->frontend, 0, ZMQ_POLLIN | ZMQ_POLLOUT, 0};

	while (isWorking) {
		bool didWork = false;
		auto now = std::chrono::high_resolution_clock::now();

		try {
			zmq::poll(&pollContext, 1, 10);
		} catch (zmq::error_t & ex) {
			printf("ZMQ failed [%s] zmq::poll - stopping client.\n", ex.what());
			return;
		}

		if (pollContext.revents & ZMQ_POLLIN) {
			didWork = true;

			for (int c = 0; c < MAX_CONSEQ_MESSAGES && isWorking; ++c) {
				zmq::message_t controlMsg, payloadMsg;
				try {
					this->frontend->recv(&controlMsg);
					this->frontend->recv(&payloadMsg);
				} catch (zmq::error_t & ex) {
					printf("ZMQ failed [%s] zmq::socket_t::recv - stopping client.\n", ex.what());
					return;
				}

				ControlFrame frame(controlMsg);

				if (!frame) {
					printf("ZMQ expected protocol version [%d], server speaks [%d], dropping message.\n", ZMQ_PROTOCOL_VERSION, frame.version);
					continue;
				}

				if (frame.type != clientType) {
					puts("ZMQ server sent mismatching msg type of worker for us!");
					continue;
				}

				lastHBRecv = std::chrono::high_resolution_clock::now();

				if (frame.control == ControlMessage::DATA_MSG) {
					std::lock_guard<std::mutex> cbLock(callbackMutex);
					if (this->callback) {
						this->callback(VRayMessage::fromZmqMessage(payloadMsg), this);
					}
				} else if (frame.control == ControlMessage::PING_MSG) {
					if (payloadMsg.size() != 0) {
						puts("ZMQ missing empty frame after ping");
					}
				} else if (frame.control == ControlMessage::PONG_MSG) {
					if (payloadMsg.size() != 0) {
						puts("ZMQ missing empty frame after pong");
					}
				}

				int more = 0;
				size_t more_size = sizeof (more);
				try {
					frontend->getsockopt(ZMQ_RCVMORE, &more, &more_size);
				} catch (zmq::error_t & ex) {
					printf("ZMQ failed [%s] zmq::socket_t::getsockopt.\n", ex.what());
				}
				if (!more) {
					break;
				}
			}
		}

		if (pollContext.revents & ZMQ_POLLOUT) {
			try {
				now = std::chrono::high_resolution_clock::now();
				// we havent sent messages in a while - ping server
				if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastHBSend).count() > CLIENT_PING_INTERVAL) {
					bool sent = frontend->send(ControlFrame::make(clientType, ControlMessage::PING_MSG), ZMQ_SNDMORE);
					if (sent) {
						sent = frontend->send(emptyFrame);
						lastHBSend = now;
						didWork = true;
					}
				}

				didWork = didWork || !messageQue.empty();
				workerSendoutMessages(lastHBSend);
			} catch (zmq::error_t & ex) {
				printf("ZMQ failed [%s] zmq::socket_t::send - stopping client.\n", ex.what());
				return;
			}
		}

		if (clientType == ClientType::Heartbeat && std::chrono::duration_cast<std::chrono::milliseconds>(now - lastHBRecv).count() > HEARBEAT_TIMEOUT) {
			puts("ZMQ server unresponsive, stopping client");
			return;
		}

		if (!didWork && isWorking) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}

	if (serverStop) {
		try {
			int wait = 200;
			frontend->setsockopt(ZMQ_SNDTIMEO, &wait, sizeof(wait));
			frontend->send(ControlFrame::make(clientType, ControlMessage::STOP_MSG), ZMQ_SNDMORE);
			frontend->send(emptyFrame);
			serverStop = false;
		} catch (zmq::error_t & ex) {
			printf("ZMQ exception while stopping server: %s\n", ex.what());
		}
	} else if (flushOnExit) {
		try {
			int wait = 200;
			this->frontend->setsockopt(ZMQ_SNDTIMEO, &wait, sizeof(wait));
			std::lock_guard<std::mutex> lock(this->messageMutex);

			for (int c = 0; c < this->messageQue.size(); ++c) {
				auto & msg = this->messageQue[c];
				bool sent = frontend->send(ControlFrame::make(), ZMQ_SNDMORE);
				sent = sent && this->frontend->send(msg);
				if (!sent) {
					break;
				}
			}

			this->frontend->close();
		} catch (zmq::error_t & ex) {
			printf("ZMQ exception while flushing on exit: %s\n", ex.what());
		}
	}
}

inline bool ZmqClient::workerSendoutMessages(time_point & lastHBSend) {
	bool didWork = false;
	for (int c = 0; c < MAX_CONSEQ_MESSAGES && !this->messageQue.empty() && isWorking; ++c) {
		didWork = true;
		std::lock_guard<std::mutex> lock(this->messageMutex);
		auto & msg = this->messageQue.front();

		bool sent = frontend->send(ControlFrame::make(ClientType::Exporter, ControlMessage::DATA_MSG), ZMQ_SNDMORE);
		if (sent) {
			sent = frontend->send(msg);
			// update hb send since we sent a message
			lastHBSend = std::chrono::high_resolution_clock::now();
			this->messageQue.pop_front();

			int more = 0;
			size_t more_size = sizeof (more);
			frontend->getsockopt(ZMQ_RCVMORE, &more, &more_size);
			if (!more) {
				break;
			}
		} else {
			break;
		}

	}

	return didWork;
}

inline void ZmqClient::connect(const char * addr) {
	std::random_device device;
	std::mt19937_64 generator(device());
	uint64_t id = generator();

	this->frontend->setsockopt(ZMQ_IDENTITY, &id, sizeof(id));

	try {
		this->frontend->connect(addr);
	} catch (zmq::error_t & e) {
		printf("ZMQ zmq::socket_t::connect(%s) exception: %s\n", addr, e.what());
		this->errorConnect = true;
	}

	{
		std::lock_guard<std::mutex> lock(startServingMutex);
		this->startServing = true;
	}
	startServingCond.notify_one();
}

inline int ZmqClient::getOutstandingMessages() const {
	return this->messageQue.size();
}

inline bool ZmqClient::connected() const {
	return this->startServing && !this->errorConnect;
}

inline bool ZmqClient::good() const {
	return this->isWorking;
}

inline void ZmqClient::stopServer() {
	serverStop = true;
	isWorking = false;
}

inline bool ZmqClient::waitForMessages(int timeout) {
	timeout = std::min(timeout, 10000);
	using namespace std::chrono;
	{
		std::lock_guard<std::mutex> lock(this->messageMutex);
		if (this->messageQue.empty()) {
			return true;
		}
	}

	const auto waitBegin = high_resolution_clock::now();

	while (isWorking) {
		std::lock_guard<std::mutex> lock(this->messageMutex);
		if (this->messageQue.empty()) {
			return true;
		}
		const auto timePassed = duration_cast<milliseconds>(high_resolution_clock::now() - waitBegin).count();
		if (timePassed >= timeout) {
			return false;
		}
	}

	return false;
}

inline void ZmqClient::syncStop() {
	using namespace std::chrono;
	if (serverStop) {
		// give chance for worker to send stop message
		auto stopBegin = high_resolution_clock::now();
		while (serverStop) {
			if (duration_cast<milliseconds>(high_resolution_clock::now() - stopBegin).count() > 200) {
				break;
			}
			std::this_thread::sleep_for(milliseconds(1));
		}
	}

	{
		std::lock_guard<std::mutex> lock(startServingMutex);
		isWorking = false;
		startServing = true;
		startServingCond.notify_all();
	}

	context.close();
	if (worker.joinable()) {
		worker.join();
	}
	worker = std::thread();
}

inline ZmqClient::~ZmqClient() {
	this->syncStop();
}

inline void ZmqClient::setFlushOnExit(bool flag) {
	flushOnExit = flag;
}

inline bool ZmqClient::getFlushOnexit() const {
	return flushOnExit;
}

inline void ZmqClient::setCallback(ZmqOnMessageCallback cb) {
	std::lock_guard<std::mutex> cbLock(callbackMutex);
	this->callback = cb;
}

inline void ZmqClient::send(zmq::message_t && message) {
	std::lock_guard<std::mutex> lock(this->messageMutex);
	this->messageQue.push_back(std::move(message));
}

inline void ZmqClient::send(const void * data, int size) {
	zmq::message_t msg(data, size);

	std::lock_guard<std::mutex> lock(this->messageMutex);
	this->messageQue.push_back(std::move(msg));
}


#endif // _ZMQ_WRAPPER_H_
