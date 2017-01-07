#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <windows.h>
#include <WinSock2.h>
#include <iostream>
#include <thread>
#include <vector>
#include <list>
#include <math.h>
#include "protocol.h"

#pragma comment(lib, "ws2_32")

using namespace std;

#define NUM_OF_THREAD      8

#define OP_RECV            1
#define OP_SEND            2

void error_display(char *msg, int err_num)
{
	LPVOID lpMsgBuf;
	FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
		NULL, err_num,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)&lpMsgBuf, 0, NULL);
	printf("[%s] %s", msg, (char *)lpMsgBuf);
	LocalFree(lpMsgBuf);
}

typedef struct PLAYER {
	int         x;
	int         z;
	list<int>	viewList;
} PLAYER;

typedef struct OverlapEx {
	WSAOVERLAPPED	original_overlap;
	int				operation;
	WSABUF			recv_buff;
	unsigned char	socket_buff[MAX_BUFF_SIZE];
	int				packet_size;

}OverlapEx;

// 클라이언트는 확장 오버랩 구조체를 가져야 한다.
typedef struct CLIENT {
	int				id;
	bool			is_connected;
	SOCKET			sock;
	PLAYER			player;
	OverlapEx		recv_overlap;
	int				previous_data_size;
	unsigned char	packet[MAX_BUFF_SIZE];      //조각난 패킷들이 조합되는 공간

}CLIENT;

CLIENT clients[MAX_USER];
bool g_isShutDown = false;
HANDLE g_hIOCP;
//int g_ObjectListSector[Row][Col]
CRITICAL_SECTION g_CriticalSection;


void Initialize_Server()
{
//	_wsetlocale(LC_ALL, L"korean");
	WSADATA   wsadata;
	WSAStartup(MAKEWORD(2, 2), &wsadata);

	InitializeCriticalSection(&g_CriticalSection);
	g_hIOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, 0);

	for (int i = 0; i < MAX_USER; ++i) {
		clients[i].recv_overlap.recv_buff.buf = reinterpret_cast<CHAR *>(clients[i].recv_overlap.socket_buff);
		clients[i].recv_overlap.recv_buff.len = MAX_BUFF_SIZE;
		clients[i].recv_overlap.operation = OP_RECV;

		clients[i].is_connected = false;
	}
}

void SendPacket(int id, unsigned char* packet) {

	// 아래처럼 지역변수로 놔두면 안된다. 데이터 다 보내기도 전에 소멸되므로
	// OverlapEx send_over;

	OverlapEx* send_over = new OverlapEx;
	memset(send_over, 0, sizeof(OverlapEx));			// 꼭 해줘야한다!!!!!!!!!!!
	send_over->operation = OP_SEND;
	send_over->recv_buff.buf = reinterpret_cast<CHAR*>(send_over->socket_buff);
	send_over->recv_buff.len = packet[0];
	memcpy(send_over->socket_buff, packet, packet[0]);

	int result = WSASend(clients[id].sock, &send_over->recv_buff, 1, NULL, 0, &send_over->original_overlap, NULL);
	if ((0 != result) && (WSA_IO_PENDING != result)) {
		int error_num = WSAGetLastError();

		if (WSA_IO_PENDING != error_num)
			error_display("SendPacket : WSASend", error_num);
		while (true);
	}
}

void ProcessPacket(int now_ID, unsigned char * packet) {
	// 패킷 종류별로 처리가 달라진다.
	// [0] : size, [1] : type
	int dx = clients[now_ID].player.x;
	int dz = clients[now_ID].player.z;
	unsigned char packet_type = packet[1];

	switch (packet_type) {
	case CS_UP: dz--; break;
	case CS_DOWN: dz++; break;
	case CS_LEFT: dx--; break;
	case CS_RIGHT: dx++; break;
	default: cout << "Unknown Packet Type Detexcted!!" << endl;
		exit(-1);
	}

	if (dz < 0) dz = 0;
	if (dz >= BOARD_HEIGHT) dz = BOARD_HEIGHT;
	if (dx < 0) dx = 0;
	if (dx >= BOARD_WIDTH) dx = BOARD_WIDTH;

	// 이동 처리
	clients[now_ID].player.x = dx;
	clients[now_ID].player.z = dz;


	sc_packet_pos pos_packet;
	sc_packet_put_player putPlayer_packek;
	sc_packet_remove_player removePlayer_packet;

	
	pos_packet.id = now_ID;
	pos_packet.size = sizeof(sc_packet_pos);
	pos_packet.type = SC_POS;
	pos_packet.x = dx;
	pos_packet.y = dz;

	SendPacket(now_ID, reinterpret_cast<unsigned char*>(&pos_packet));

	/*
	for (int i = 0; i<MAX_USER; ++i) {
		if (false == clients[i].is_connected) continue;
			SendPacket(i, reinterpret_cast<unsigned char*>(&pos_packet));
	}
	*/


	// 시야 처리
	list<int> nearList;
	list<int> removeList;

	for (int i = 0; i < MAX_USER; ++i) {
		if (false == clients[i].is_connected) continue;
		if (i == now_ID) continue;

		if (abs(clients[now_ID].player.x - clients[i].player.x) <= VIEW_RADIUS)
			if (abs(clients[now_ID].player.z - clients[i].player.z) <= VIEW_RADIUS) {
				nearList.push_back(i);
			}
	}

	// 모든 근처의 객체들에 대하여
	for (auto& nearClientID : nearList) {
		auto findNearClient = find(clients[now_ID].player.viewList.begin(), clients[now_ID].player.viewList.end(), nearClientID);

		// 존재하지 않는다면
		if (findNearClient == clients[now_ID].player.viewList.end()) {
			EnterCriticalSection(&g_CriticalSection);
			clients[now_ID].player.viewList.push_back(nearClientID);
			LeaveCriticalSection(&g_CriticalSection);
			// 나에게 상대 추가
			putPlayer_packek.id = nearClientID;
			putPlayer_packek.size = sizeof(sc_packet_put_player);
			putPlayer_packek.type = SC_PUT_PLAYER;
			putPlayer_packek.x = clients[nearClientID].player.x;
			putPlayer_packek.y = clients[nearClientID].player.z;

			SendPacket(now_ID, reinterpret_cast<unsigned char*>(&putPlayer_packek));

			auto findNearClient = find(clients[nearClientID].player.viewList.begin(), clients[nearClientID].player.viewList.end(), now_ID);

			if (findNearClient == clients[nearClientID].player.viewList.end()) {
				EnterCriticalSection(&g_CriticalSection);
				clients[nearClientID].player.viewList.push_back(now_ID);
				LeaveCriticalSection(&g_CriticalSection);

				putPlayer_packek.id = now_ID;
				putPlayer_packek.size = sizeof(sc_packet_put_player);
				putPlayer_packek.type = SC_PUT_PLAYER;
				putPlayer_packek.x = clients[now_ID].player.x;
				putPlayer_packek.y = clients[now_ID].player.z;

				SendPacket(nearClientID, reinterpret_cast<unsigned char*>(&putPlayer_packek));
			}
			else {
				pos_packet.id = now_ID;
				pos_packet.size = sizeof(sc_packet_pos);
				pos_packet.type = SC_POS;
				pos_packet.x = clients[now_ID].player.x;
				pos_packet.y = clients[now_ID].player.z;

				SendPacket(nearClientID, reinterpret_cast<unsigned char*>(&pos_packet));
			}
		}
		else {
			auto findNearClient = find(clients[nearClientID].player.viewList.begin(), clients[nearClientID].player.viewList.end(), now_ID);

			if (findNearClient == clients[nearClientID].player.viewList.end()) {
				EnterCriticalSection(&g_CriticalSection);
				clients[nearClientID].player.viewList.push_back(now_ID);
				LeaveCriticalSection(&g_CriticalSection);
				
				putPlayer_packek.id = now_ID;
				putPlayer_packek.size = sizeof(sc_packet_put_player);
				putPlayer_packek.type = SC_PUT_PLAYER;
				putPlayer_packek.x = clients[now_ID].player.x;
				putPlayer_packek.y = clients[now_ID].player.z;

				SendPacket(nearClientID, reinterpret_cast<unsigned char*>(&putPlayer_packek));
			}
			else {
				//	cout << "상대는 내가 그대로 움직이고 있다." << endl;
				pos_packet.id = now_ID;
				pos_packet.size = sizeof(sc_packet_pos);
				pos_packet.type = SC_POS;
				pos_packet.x = clients[now_ID].player.x;
				pos_packet.y = clients[now_ID].player.z;

				SendPacket(nearClientID, reinterpret_cast<unsigned char*>(&pos_packet));
			}
		}
	}
	// 시야에서 사라짐
	for (auto& myViewClientID : clients[now_ID].player.viewList) {
		auto findNearClient = find(nearList.begin(), nearList.end(), myViewClientID);

		if (findNearClient == nearList.end()) {
			removeList.push_back(myViewClientID);
		}
	}

	for (auto& removeClientID : removeList) {
		auto removeValue = find(clients[now_ID].player.viewList.begin(), clients[now_ID].player.viewList.end(), removeClientID);

		EnterCriticalSection(&g_CriticalSection);
		clients[now_ID].player.viewList.erase(removeValue);
		LeaveCriticalSection(&g_CriticalSection);
	
		removePlayer_packet.id = removeClientID;
		removePlayer_packet.size = sizeof(sc_packet_remove_player);
		removePlayer_packet.type = SC_REMOVE_PLAYER;

		SendPacket(now_ID, reinterpret_cast<unsigned char*>(&removePlayer_packet));

		for (int i = 0; i < MAX_USER; ++i) {
			if (false == clients[i].is_connected) continue;
			if (i == now_ID) continue;
			if (removeClientID == clients[i].id) {

				auto other_removeValue = find(clients[i].player.viewList.begin(), clients[i].player.viewList.end(), now_ID);

				if (other_removeValue != clients[i].player.viewList.end()) {
					EnterCriticalSection(&g_CriticalSection);
					clients[i].player.viewList.erase(other_removeValue);
					LeaveCriticalSection(&g_CriticalSection);

					removePlayer_packet.id = now_ID;
					removePlayer_packet.size = sizeof(sc_packet_remove_player);
					removePlayer_packet.type = SC_REMOVE_PLAYER;

					SendPacket(i, reinterpret_cast<unsigned char*>(&removePlayer_packet));
				}
			}
		}
	}

	/*
	for (int i = 0; i < MAX_USER; ++i) {
		if (clients[i].is_connected == true) {
			cout << "-------------------- player "<< i << " ------------------------ " << endl;
			cout << "Position  : " << clients[i].player.x << ", " << clients[i].player.z << endl;
			cout << "View List : ";
			for (auto j : clients[i].player.viewList)
				cout << j << " ";
			cout << endl;
		}
	}	
	cout << endl;
	*/

	nearList.clear();
	removeList.clear();
}

void Accept_Thread()
{
	sockaddr_in listen_addr;
	// Overlapped 소켓으로 받을 것이므로 옵션 추가
	SOCKET accept_socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);

	ZeroMemory(&listen_addr, sizeof(listen_addr));
	listen_addr.sin_family = AF_INET;
	listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);      //모든 클라이언트의 주소에서 받아라
	listen_addr.sin_port = htons(MY_SERVER_PORT);
	ZeroMemory(&listen_addr.sin_zero, 8);

	//C++ 의 bind와 socket programing의 bind가 충돌한다
	::bind(accept_socket, reinterpret_cast<SOCKADDR*>(&listen_addr), sizeof(listen_addr));

	listen(accept_socket, 10);

	while (true) {
		sockaddr_in client_addr;
		int add_size = sizeof(client_addr);

		SOCKET new_client = ::WSAAccept(accept_socket, reinterpret_cast<SOCKADDR*>(&client_addr), &add_size, NULL, NULL);

		// 새로운 아이디 할당
		int new_id = -1;
		for (int i = 0; i < MAX_USER; ++i) {
			if (false == clients[i].is_connected) {
				new_id = i;
				break;
			}
		}


		if (-1 == new_id) {
			cout << "The Server is Full of User" << endl;
			closesocket(new_client);
			continue;
		}
		// 입출력 포트와 클라이언트 연결
		CreateIoCompletionPort(reinterpret_cast<HANDLE>(new_client), g_hIOCP, new_id, 0);

		cout << "Player " << new_id << " Connected " << endl;

		EnterCriticalSection(&g_CriticalSection);
		// 재활용 될 소켓이므로 초기화해주어야 한다.
		clients[new_id].sock = new_client;
		clients[new_id].is_connected = true;
		clients[new_id].id = new_id;

		// DB에서 이전에 로그아웃 한 위치로 다시 재접속
		clients[new_id].player.x = 4;
		clients[new_id].player.z = 4;
		clients[new_id].recv_overlap.operation = OP_RECV;
		clients[new_id].recv_overlap.packet_size = 0;
		clients[new_id].previous_data_size = 0;
		LeaveCriticalSection(&g_CriticalSection);
		
		sc_packet_put_player put_player_pacekt;
		put_player_pacekt.id = new_id;
		put_player_pacekt.size = sizeof(put_player_pacekt);
		put_player_pacekt.type = SC_PUT_PLAYER;
		put_player_pacekt.x = clients[new_id].player.x;
		put_player_pacekt.y = clients[new_id].player.z;
	
		// 플레이어 접속 알림
		for (int i = 0; i < MAX_USER; ++i) {
			if (true == clients[i].is_connected) {
				SendPacket(i, reinterpret_cast<unsigned char*>(&put_player_pacekt));
			}
		}

		// 시야 처리
		for (int i = 0; i < MAX_USER; ++i) {
			if (false == clients[i].is_connected) continue;
			if (i == new_id) continue;

			if (abs(clients[i].player.x - clients[new_id].player.x) <= VIEW_RADIUS)
				if (abs(clients[i].player.z - clients[new_id].player.z) <= VIEW_RADIUS) {
					EnterCriticalSection(&g_CriticalSection);
					clients[new_id].player.viewList.push_back(i);
					LeaveCriticalSection(&g_CriticalSection);
				}
		}

		for (auto& i : clients[new_id].player.viewList) {
			// 다른 플레이어들의 뷰 리스트에 추가
			EnterCriticalSection(&g_CriticalSection);
			clients[i].player.viewList.push_back(new_id);
			LeaveCriticalSection(&g_CriticalSection);

			put_player_pacekt.id = i;
			put_player_pacekt.x = clients[i].player.x;
			put_player_pacekt.y = clients[i].player.z;
			SendPacket(new_id, reinterpret_cast<unsigned char *> (&put_player_pacekt));
		}
			
		/*
		// 나를 제외한 다른 플레이어들의 위치 확인
		for (int i = 0; i < MAX_USER; ++i) {
			if (false == clients[i].is_connected) continue;
			if (i == new_id) continue;

			put_player_pacekt.id = i;
			put_player_pacekt.x = clients[i].player.x;
			put_player_pacekt.y = clients[i].player.z;
			SendPacket(new_id, reinterpret_cast<unsigned char *> (&put_player_pacekt));
		
		}
		*/

		DWORD flags = 0;
		int result = WSARecv(new_client, &clients[new_id].recv_overlap.recv_buff, 1, NULL, &flags, &clients[new_id].recv_overlap.original_overlap, NULL);

		if (0 != result) {
			int error_num = WSAGetLastError();
			if (WSA_IO_PENDING != error_num) {
				error_display("AcceptThread : WSARecv ", error_num);
			}
		}
	}
}

void Worker_Thread() {
	DWORD io_Size;
	DWORD key;
	OverlapEx* overlap;
	bool bResult;

	while (true) {
		bResult = GetQueuedCompletionStatus(g_hIOCP, &io_Size, &key, reinterpret_cast<LPOVERLAPPED*>(&overlap), INFINITE);
		if (false == bResult) {
			//Error 처리
		//	cout << "Worker_Thread Error - GetQueuedCompletionStatus 의 결과가 false" << endl;
		}
		if (0 == io_Size) {
			closesocket(clients[key].sock);

			sc_packet_remove_player rp_packet;
			rp_packet.id = key;
			rp_packet.size = sizeof(sc_packet_remove_player);
			rp_packet.type = SC_REMOVE_PLAYER;

			for (auto i = 0; i < MAX_USER; ++i) {
				if (false == clients[i].is_connected) continue;
				if (key == i) continue;

				SendPacket(i, reinterpret_cast<unsigned char*>(&rp_packet));
			}
			clients[key].is_connected = false;
			continue;
		}
		switch (overlap->operation) {
		case OP_RECV: {
			unsigned char* pBuff = overlap->socket_buff;
			int remained = io_Size;

			//남은 데이터 사이즈만큼 순회하면서 처리해라.
			while (0 < remained) {
				if (clients[key].recv_overlap.packet_size == 0) {
					clients[key].recv_overlap.packet_size = pBuff[0];      //모든 패킷의 맨 앞 칸은 사이즈이다.
				}

				// 이전에 받은 패킷과 총량을 비교하여 남은 데이터 조사
				int required = clients[key].recv_overlap.packet_size - clients[key].previous_data_size;

				// 패킷 완성
				if (remained >= required) {
					//지난번에 받은 데이터 뒷부분에 복사
					memcpy(clients[key].packet + clients[key].previous_data_size, pBuff, required);
					ProcessPacket(key, clients[key].packet);
					remained -= required;
					pBuff += required;
					clients[key].recv_overlap.packet_size = 0;
					clients[key].previous_data_size = 0;
				}
				else {
					memcpy(clients[key].packet + clients[key].previous_data_size, pBuff, remained);
					//미완성 패킷의 사이즈가 remained만큼 증가했다.
					clients[key].previous_data_size += remained;
					remained = 0;
					pBuff++;
				}
			}
			DWORD flags = 0;
			WSARecv(clients[key].sock, &clients[key].recv_overlap.recv_buff, 1, NULL, &flags, reinterpret_cast<LPWSAOVERLAPPED>(&clients[key].recv_overlap), NULL);

			break;
		}
		case OP_SEND:
			// ioSize하고 실제 보낸 크기 비교문 넣으라함 if()
			// 비교 후 소켓 접속 끊기

			//IO 사이즈와 실제 보낸 크기 비교
			delete overlap;
			break;
		default:
			cout << "Unknown Event on Worker_Thread" << endl;
			while (true);
			break;
		}
	}
}

int main() {
	thread* pAcceptThread;
	vector<thread*> vpThread;

	Initialize_Server();

	for (int i = 0; i<NUM_OF_THREAD; ++i)
		vpThread.push_back(new thread{ Worker_Thread });

	pAcceptThread = new thread{ Accept_Thread };

	while (g_isShutDown == false) {
		Sleep(1000);
	}

	for (thread* pThread : vpThread) {
		pThread->join();
		delete pThread;
	}

	pAcceptThread->join();
	delete pAcceptThread;

	DeleteCriticalSection(&g_CriticalSection);

	WSACleanup();
}
