#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>

#include <LeapC.h>										// LeapMotion SDK
#include <windows.h>									// Just used multithread
#include <iostream>										// std io
#include <sstream>										// string buffer
#include <map>										// dynamic array

/*================================================================================
Maintenance & Update Log
Dec 17, 2018: Add arm info into the string.
================================================================================*/

using std::cout;
using std::cin;
using std::endl;
using std::stringstream;

DWORD WINAPI leapServiceLoop(void *param);				// Leap Service thread
DWORD WINAPI websocketServiceLoop(void *param);
void OnFrame(const LEAP_TRACKING_EVENT *frame);			// Called when hand tracking
void swapYZ(LEAP_VECTOR &vec);							// Swap YZ coordinates
void OutputFingerBonePos(std::stringstream &out, const LEAP_BONE &bone);
void OutputFingerBoneDir(std::stringstream &out, const LEAP_BONE &bone);
void OutputBoneOrientation(std::stringstream &out, const LEAP_BONE &bone);

typedef websocketpp::config::asio cur_config;
typedef websocketpp::server<cur_config> wsserver;

using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;
using websocketpp::lib::bind;

// When open new connection
void websocket_open_handler(wsserver *s, websocketpp::connection_hdl hdl);
// When a connection is closed
void websocket_close_handler(wsserver *s, websocketpp::connection_hdl hdl);

// LeapVector Output override
std::ostream& operator<<(std::ostream &os, const LEAP_VECTOR &vec)
{
	os << vec.x << "," << vec.y << "," << vec.z;
	return os;
}

bool _isRunning = false;								// global flag for running the thread
wsserver server;										// global server endpoint
std::map<std::string, websocketpp::connection_hdl> connections;
CRITICAL_SECTION csconnections;							// for mutex

// Main Function, establish Leap Connection, create Leap Service Thread and keep tracking
int main()
{
	HANDLE hLeapThread = 0;
	HANDLE hWebThread = 0;

	InitializeCriticalSection(&csconnections);

	// Start service
	cout << "Establishing Connection" << endl;
	// Create Connection
	LEAP_CONNECTION connection;
	eLeapRS ret = LeapCreateConnection(0, &connection);
	if (ret == eLeapRS_Success) {
		cout << "Connection created successfully" << endl;
		ret = LeapOpenConnection(connection);

		if (ret == eLeapRS_Success) {

			// Start Message Poll
			_isRunning = true;
			hLeapThread = CreateThread(NULL, 0, leapServiceLoop, &connection, 0, 0);
			cout << "Leap Service started" << endl;

			// Start Websocket
			hWebThread = CreateThread(NULL, 0, websocketServiceLoop, 0, 0, 0);
		}

		// Let user press q to exit
		cout << "Press q to exit" << endl;
		while (true) {
			char c;
			cin >> c;
			if (c == 'q')
				break;
		}

		// Now quit
		_isRunning = false;
		// Close Leap Connection
		LeapCloseConnection(connection);
		// Destroy Connection
		LeapDestroyConnection(connection);
		// Close connection must be before Wait For thread. Because the thread is probably 
		// blocked by inquirying message forever.

		// Wait for thread to stop
		WaitForSingleObject(hLeapThread, INFINITE);
		CloseHandle(hLeapThread);

		// Stop server
		server.stop();
		WaitForSingleObject(hWebThread, INFINITE);
		CloseHandle(hWebThread);

		// Stop mutex
		DeleteCriticalSection(&csconnections);
		
	}

	return 0;
}

DWORD WINAPI leapServiceLoop(void *param)
{
	eLeapRS ret;
	LEAP_CONNECTION_MESSAGE msg;
	LEAP_CONNECTION *connection = (LEAP_CONNECTION*)param;

	// set HMD policy
	LeapSetPolicyFlags(*connection, eLeapPolicyFlag::eLeapPolicyFlag_OptimizeHMD |
        eLeapPolicyFlag::eLeapPolicyFlag_BackgroundFrames, 0);
	cout << "Setting to HMD mode." << endl;

	while (_isRunning) {
		// keep running
		ret = LeapPollConnection(*connection, 1000, &msg);
		// if not succeed
		if (ret != eLeapRS_Success) {
			cout << "LeapC Pollconnection call failed." << endl;
			continue;
		}

		// Deal with messages
		switch (msg.type) {
		case eLeapEventType_Device:
			cout << "new device detected" << endl;
			break;
		case eLeapEventType_Tracking:
			OnFrame(msg.tracking_event);
			break;
		case eLeapEventType_Policy:
            cout << "policy event received" << endl;
			// Get current policy
			if (msg.policy_event->current_policy & eLeapPolicyFlag::eLeapPolicyFlag_OptimizeHMD) {
				cout << "HMD is set." << endl;
			}
		default:
			break;
		}
	}
	return 0;
}

DWORD WINAPI websocketServiceLoop(void *param)
{
	// Create a server
	try {
		server.set_access_channels(websocketpp::log::alevel::all);
		server.clear_access_channels(websocketpp::log::alevel::frame_payload | websocketpp::log::alevel::frame_header);

		server.init_asio();
		server.set_open_handler(bind(&websocket_open_handler, &server, ::_1));
		server.set_close_handler(bind(&websocket_close_handler, &server, ::_1));

		server.listen(9999);
		server.start_accept();
		cout << "Start listening 9999" << endl;
		server.run();
		cout << "Websocket server stops" << endl;
	}
	catch (websocketpp::exception const & e) {
		cout << e.what() << endl;
	}
	catch (...) {
		cout << "Some unknown exception occured" << endl;
	}

	return 0;
}

void websocket_open_handler(wsserver *s, websocketpp::connection_hdl hdl)
{
	cout << "New connection opened" << endl;
	std::shared_ptr<websocketpp::connection<cur_config>> con = s->get_con_from_hdl(hdl);
	std::string remote = con->get_remote_endpoint();
	connections.insert(std::pair<std::string, websocketpp::connection_hdl>(remote, hdl));
}

void websocket_close_handler(wsserver *s, websocketpp::connection_hdl hdl)
{
	cout << "A connection is closed" << endl;
	// Find and remove
	EnterCriticalSection(&csconnections);
	// Try to find
	auto con = s->get_con_from_hdl(hdl);
	auto i = connections.find(con->get_remote_endpoint());
	if (i != connections.end()) {
		connections.erase(i);
		cout << "Remove connection." << endl;
	}
	LeaveCriticalSection(&csconnections);
}

void OnFrame(const LEAP_TRACKING_EVENT *frame)
{
	// Tracking.
	// Take Hand
	stringstream ss("");
	if (frame->nHands > 0) {
		for (unsigned int i = 0; i < 2 && i < frame->nHands; ++i) {
			if (i > 0) {
				// Means more than one hand
				ss << "#OneMore#";
			}

			// Convert Hand to String
			auto hand = frame->pHands[i];
			// Hand type
			if (hand.type == eLeapHandType::eLeapHandType_Left)
				ss << "hand_type: left; ";
			else
				ss << "hand_type: right; ";
			// Hand palm position
			swapYZ(hand.palm.position);
			ss << "palm_pos: " << hand.palm.position << "; ";
			// Hand palm velocity
			ss << "palm_vel: " << hand.palm.velocity << "; ";
			// Hand palm normal
			swapYZ(hand.palm.normal);
			ss << "palm_norm: " << hand.palm.normal << "; ";
			// Palm Direction
			swapYZ(hand.palm.direction);
			ss << "palm_dir: " << hand.palm.direction << "; ";
			// For each fingers
			for (unsigned int f = 0; f < 5; ++f) {
				/*
				0 = THUMB
				1 = INDEX
				2 = MIDDLE
				3 = RING
				4 = PINKY
				*/
				auto finger = hand.digits[f];
				ss << "finger_type: " << finger.finger_id << "; ";
				for (unsigned int b = 1; b < 4; ++b) {
					ss << "finger_" << b << "_pos: ";
					OutputFingerBonePos(ss, finger.bones[b]);
					ss << "; finger_" << b << "_dir: ";
					OutputFingerBoneDir(ss, finger.bones[b]);
					ss << "; ";
				}
			}

            // Output arm
            ss << "arm_pos: ";
            OutputFingerBonePos(ss, frame->pHands->arm);
            ss << "; arm_dir: ";
            OutputFingerBoneDir(ss, frame->pHands->arm);
            // OutputBoneOrientation(ss, frame->pHands->arm);

			// No gesture output

			// Get final string
			std::string outputstr = ss.str();
			// Send to all clients
			EnterCriticalSection(&csconnections);
			for (auto iter = connections.begin(); iter != connections.end(); ++iter) {
				server.send(iter->second, outputstr, websocketpp::frame::opcode::text);
			}
			LeaveCriticalSection(&csconnections);

			ss.str("");
			ss.clear();
		}
	}
}

void swapYZ(LEAP_VECTOR& vec)
{
	vec.x = -vec.x;
	float tmp = vec.y;
	vec.y = -vec.z;
	vec.z = -tmp;
}

void OutputFingerBonePos(std::stringstream &out, const LEAP_BONE &bone)
{
	// Output center point (YZ swaped)
	float x, y, z;
	x = (bone.next_joint.x + bone.prev_joint.x) / 2.f;
	y = (bone.next_joint.y + bone.prev_joint.y) / 2.f;
	z = (bone.next_joint.z + bone.prev_joint.z) / 2.f;
	out << -x << "," << -z << "," << -y;
}

void OutputFingerBoneDir(std::stringstream &out, const LEAP_BONE &bone)
{
	// Output finger direction (XY swaped)
	float x, y, z;
	x = bone.next_joint.x - bone.prev_joint.x;
	y = bone.next_joint.y - bone.prev_joint.y;
	z = bone.next_joint.z - bone.prev_joint.z;
	out << x << "," << z << "," << -y;
}

void OutputBoneOrientation(std::stringstream &out, const LEAP_BONE &bone)
{
    // Output x,y,z,w four comp
    LEAP_QUATERNION const *rot = &bone.rotation;
    out << rot->x << "," << rot->y << "," << rot->z << "," << rot->w;
}